#include <net/if.h>
#include <signal.h>

#include <charconv>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include "control/XdpConfigSync.h"
#include "loader.h"

namespace {

struct Options {
  bool attach = false;
  bool mapsMode = false;
  std::vector<std::string> endpoints;
  std::vector<XdpBackendValue> backends;
  std::string device;
  std::string object;
  l4lb::xdp::Mode mode = l4lb::xdp::Mode::Generic;
  uint32_t program_id = 0;
};

void usage() {
  std::cout
      << "用法：\n"
         "  l4lb-xdp attach --dev NAME --object PATH [--mode generic|native]\n"
         "    maps 模式：追加 --maps [--backend IPv4:PORT]...\n"
         "  l4lb-xdp detach --dev NAME --prog-id ID [--mode generic|native]\n"
         "  l4lb-xdp --help\n"
         "默认 generic；attach 前台等待 SIGINT/SIGTERM，然后条件卸载。\n"
         "不会覆盖已有程序；detach 必须指定预期程序 ID。\n";
}

Options parse(int argc, char** argv) {
  if (argc < 2) throw std::invalid_argument("缺少 attach/detach 子命令");
  Options options;
  std::string command = argv[1];
  if (command != "attach" && command != "detach") {
    throw std::invalid_argument("未知子命令：" + command);
  }
  options.attach = command == "attach";
  bool seen_mode = false;
  bool seen_id = false;
  for (int i = 2; i < argc; i += 2) {
    std::string key = argv[i];
    if (key == "--maps" && options.attach && !options.mapsMode) {
      options.mapsMode = true;
      --i;
      continue;
    }
    if (i + 1 >= argc || argv[i + 1][0] == '\0') {
      throw std::invalid_argument("选项缺少值：" + key);
    }
    std::string value = argv[i + 1];
    if (key == "--backend" && options.attach) {
      options.endpoints.push_back(value);
    } else if (key == "--dev" && options.device.empty()) {
      options.device = value;
    } else if (key == "--object" && options.attach && options.object.empty()) {
      options.object = value;
    } else if (key == "--mode" && !seen_mode) {
      if (value != "generic" && value != "native") {
        throw std::invalid_argument("mode 只支持 generic 或 native");
      }
      options.mode = value == "generic" ? l4lb::xdp::Mode::Generic
                                        : l4lb::xdp::Mode::Native;
      seen_mode = true;
    } else if (key == "--prog-id" && !options.attach && !seen_id) {
      auto result = std::from_chars(value.data(), value.data() + value.size(),
                                    options.program_id);
      if (result.ec != std::errc{} ||
          result.ptr != value.data() + value.size() ||
          options.program_id == 0) {
        throw std::invalid_argument("prog-id 必须是正 uint32 整数");
      }
      seen_id = true;
    } else {
      throw std::invalid_argument("未知、重复或不适用的选项：" + key);
    }
  }
  if (options.device.empty() || options.device.size() >= IF_NAMESIZE ||
      options.device.find_first_of("/ \t\r\n:") != std::string::npos) {
    throw std::invalid_argument(
        "必须指定有效 --dev（1 至 15 字节，不含空白、/、:）");
  }
  if (options.attach && options.object.empty())
    throw std::invalid_argument("缺少 --object");
  if (!options.attach && !seen_id)
    throw std::invalid_argument("缺少 --prog-id");
  if (!options.mapsMode && !options.endpoints.empty())
    throw std::invalid_argument("--backend 需要 --maps");
  options.backends = l4lb::control::parseXdpBackends(options.endpoints);
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    usage();
    return std::cout ? 0 : 1;
  }
  Options options;
  try {
    options = parse(argc, argv);
  } catch (const std::invalid_argument& error) {
    std::cerr << "参数错误：" << error.what() << '\n';
    usage();
    return 2;
  }
  try {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    // SIGPIPE must not bypass RAII cleanup if the output reader disappears.
    sigaddset(&signals, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &signals, nullptr) != 0) {
      throw std::runtime_error("无法阻塞退出信号");
    }
    int ifindex = l4lb::xdp::interface_index(options.device);
    if (!options.attach) {
      l4lb::xdp::detach_program(ifindex, options.mode, options.program_id);
      std::cout << "DETACHED dev=" << options.device
                << " mode=" << l4lb::xdp::mode_name(options.mode)
                << " prog_id=" << options.program_id
                << "（已卸载或本模式无程序）" << std::endl;
      return std::cout ? 0 : 1;
    }
    l4lb::xdp::Attachment attachment;
    attachment.load(options.object, options.mapsMode, options.backends);
    attachment.attach(ifindex, options.mode);
    std::cout << "READY dev=" << options.device
              << " mode=" << l4lb::xdp::mode_name(options.mode)
              << " prog_id=" << attachment.program_id();
    if (options.mapsMode)
      std::cout << " schema=1 backend_count=" << options.backends.size();
    std::cout << std::endl;
    if (!std::cout) throw std::runtime_error("READY 输出失败，清理挂载");
    int received = 0;
    int error = sigwait(&signals, &received);
    if (error != 0)
      throw std::runtime_error("等待信号失败：" +
                               std::string(std::strerror(error)));
    bool cleanupFailed = false;
    try {
      attachment.detach();
    } catch (const std::exception& error) {
      cleanupFailed = true;
      std::cerr << "XDP 卸载失败：" << error.what() << '\n';
    }
    if (options.mapsMode) {
      try {
        auto packets = attachment.readPassPackets();
        std::cout << "XDP_STATS schema=1 pass_packets=" << packets << std::endl;
      } catch (const std::exception& error) {
        cleanupFailed = true;
        std::cerr << "XDP 统计失败：" << error.what() << '\n';
      }
    }
    if (cleanupFailed) return 1;
    std::cout << "DETACHED dev=" << options.device
              << " mode=" << l4lb::xdp::mode_name(options.mode)
              << " prog_id=" << attachment.program_id() << std::endl;
    return std::cout && received != SIGPIPE ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "XDP 失败 dev=" << options.device
              << " mode=" << l4lb::xdp::mode_name(options.mode) << "："
              << error.what() << '\n';
    return 1;
  }
}
