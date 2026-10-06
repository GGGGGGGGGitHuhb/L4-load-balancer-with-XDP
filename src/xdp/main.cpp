#include <net/if.h>
#include <signal.h>

#include <charconv>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include "XdpAttachment.h"
#include "control/RuntimeDsrService.h"
#include "control/XdpConfigSync.h"

namespace {
struct XdpOptions {
  bool attach = false;
  l4lb::xdp::XdpObjectProfile profile = l4lb::xdp::XdpObjectProfile::kLegacy;
  std::string vip;
  std::string runtimeConfig;
  std::vector<std::string> targets;
  l4lb::control::DsrConfiguration dsr;
  std::vector<std::string> endpoints;
  std::vector<XdpBackendValue> backends;
  std::string device;
  std::string object;
  l4lb::xdp::XdpAttachMode mode = l4lb::xdp::XdpAttachMode::kGeneric;
  uint32_t programId = 0;
};

void printXdpUsage() {
  std::cout
      << "用法：\n"
         "  l4lb-xdp attach --dev NAME --object PATH [--mode generic|native]\n"
         "    maps 模式：追加 --maps [--backend IPv4:PORT]...\n"
         "    DSR 模式：追加 --udp-dsr --vip IPv4:PORT [--target "
         "EGRESS@MAC]...\n"
         "    动态 DSR：--udp-dsr-runtime --vip IPv4:PORT --runtime-config "
         "PATH\n"
         "  l4lb-xdp detach --dev NAME --prog-id ID [--mode generic|native]\n"
         "  l4lb-xdp --help\n"
         "默认 generic；attach 前台等待 SIGINT/SIGTERM，然后条件卸载。\n"
         "不会覆盖已有程序；detach 必须指定预期程序 ID。\n";
}

XdpOptions parseXdpOptions(int argc, char** argv) {
  if (argc < 2) throw std::invalid_argument("缺少 attach/detach 子命令");

  XdpOptions options;
  std::string command = argv[1];
  if (command != "attach" && command != "detach") {
    throw std::invalid_argument("未知子命令：" + command);
  }
  options.attach = command == "attach";

  bool modeSeen = false;
  bool programIdSeen = false;

  for (int argumentIndex = 2; argumentIndex < argc; argumentIndex += 2) {
    std::string key = argv[argumentIndex];
    if ((key == "--maps" || key == "--udp-dsr" || key == "--udp-dsr-runtime") &&
        options.attach &&
        options.profile == l4lb::xdp::XdpObjectProfile::kLegacy) {
      options.profile = key == "--udp-dsr-runtime"
                            ? l4lb::xdp::XdpObjectProfile::kUdpRuntimeV3
                        : key == "--maps"
                            ? l4lb::xdp::XdpObjectProfile::kMapsV1
                            : l4lb::xdp::XdpObjectProfile::kUdpDsrV2;
      --argumentIndex;
      continue;
    }

    if (argumentIndex + 1 >= argc || argv[argumentIndex + 1][0] == '\0') {
      throw std::invalid_argument("选项缺少值：" + key);
    }

    std::string value = argv[argumentIndex + 1];
    if (key == "--backend" && options.attach) {
      options.endpoints.push_back(value);
    } else if (key == "--vip" && options.attach && options.vip.empty()) {
      options.vip = value;
    } else if (key == "--runtime-config" && options.attach &&
               options.runtimeConfig.empty()) {
      options.runtimeConfig = value;
    } else if (key == "--target" && options.attach) {
      options.targets.push_back(value);
    } else if (key == "--dev" && options.device.empty()) {
      options.device = value;
    } else if (key == "--object" && options.attach && options.object.empty()) {
      options.object = value;
    } else if (key == "--mode" && !modeSeen) {
      if (value != "generic" && value != "native") {
        throw std::invalid_argument("mode 只支持 generic 或 native");
      }
      options.mode = value == "generic" ? l4lb::xdp::XdpAttachMode::kGeneric
                                        : l4lb::xdp::XdpAttachMode::kNative;
      modeSeen = true;
    } else if (key == "--prog-id" && !options.attach && !programIdSeen) {
      auto result = std::from_chars(value.data(), value.data() + value.size(),
                                    options.programId);
      if (result.ec != std::errc{} ||
          result.ptr != value.data() + value.size() || options.programId == 0) {
        throw std::invalid_argument("prog-id 必须是正 uint32 整数");
      }
      programIdSeen = true;
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
  if (!options.attach && !programIdSeen)
    throw std::invalid_argument("缺少 --prog-id");

  if (options.profile != l4lb::xdp::XdpObjectProfile::kMapsV1 &&
      !options.endpoints.empty())
    throw std::invalid_argument("--backend 需要 --maps");

  options.backends = l4lb::control::parseXdpBackends(options.endpoints);

  if (options.profile == l4lb::xdp::XdpObjectProfile::kUdpRuntimeV3) {
    if (options.vip.empty() || options.runtimeConfig.empty() ||
        !options.targets.empty())
      throw std::invalid_argument(
          "动态DSR需要--vip/--runtime-config，禁止--target");
    l4lb::control::parseXdpBackends({options.vip});
  } else if (options.profile == l4lb::xdp::XdpObjectProfile::kUdpDsrV2) {
    if (options.vip.empty())
      throw std::invalid_argument("--udp-dsr 需要 --vip");
    // Literal checks remain parameter errors; interface inspection happens
    // before load.
    l4lb::control::parseXdpBackends({options.vip});
  } else if (!options.vip.empty() || !options.targets.empty()) {
    throw std::invalid_argument("--vip/--target 需要 --udp-dsr");
  }

  if (options.profile != l4lb::xdp::XdpObjectProfile::kUdpRuntimeV3 &&
      !options.runtimeConfig.empty())
    throw std::invalid_argument("--runtime-config 需要 --udp-dsr-runtime");

  return options;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    printXdpUsage();
    return std::cout ? 0 : 1;
  }

  XdpOptions options;

  try {
    options = parseXdpOptions(argc, argv);
  } catch (const std::invalid_argument& error) {
    std::cerr << "参数错误：" << error.what() << '\n';
    printXdpUsage();
    return 2;
  }

  try {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    // 输出读取者消失时，SIGPIPE 不能绕过 RAII 清理。
    sigaddset(&signals, SIGPIPE);

    if (options.profile == l4lb::xdp::XdpObjectProfile::kUdpRuntimeV3)
      sigaddset(&signals, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &signals, nullptr) != 0) {
      throw std::runtime_error("无法阻塞退出信号");
    }

    int interfaceIndex = l4lb::xdp::resolveInterfaceIndex(options.device);
    if (!options.attach) {
      l4lb::xdp::detachExpectedProgram(interfaceIndex, options.mode,
                                       options.programId);

      std::cout << "DETACHED dev=" << options.device
                << " mode=" << l4lb::xdp::xdpAttachModeName(options.mode)
                << " prog_id=" << options.programId
                << "（已卸载或本模式无程序）" << std::endl;
      return std::cout ? 0 : 1;
    }

    if (options.profile == l4lb::xdp::XdpObjectProfile::kUdpDsrV2)
      options.dsr = l4lb::control::parseDsrConfiguration(
          options.device, options.vip, options.targets);

    l4lb::xdp::XdpAttachment attachment;

    if (options.profile == l4lb::xdp::XdpObjectProfile::kUdpRuntimeV3)
      return l4lb::control::runRuntimeDsrControlLoop(
          attachment, options.object, options.device, options.vip,
          options.runtimeConfig, options.mode, signals);

    attachment.loadObject(options.object, options.profile, options.backends,
                          options.dsr);
    attachment.attachProgram(interfaceIndex, options.mode);

    std::cout << "READY dev=" << options.device
              << " mode=" << l4lb::xdp::xdpAttachModeName(options.mode)
              << " prog_id=" << attachment.programId();

    if (options.profile == l4lb::xdp::XdpObjectProfile::kMapsV1)
      std::cout << " schema=1 backend_count=" << options.backends.size();

    if (options.profile == l4lb::xdp::XdpObjectProfile::kUdpDsrV2)
      std::cout << " schema=2 profile=udp-dsr backend_count="
                << options.dsr.backends.size();

    std::cout << std::endl;
    if (!std::cout) throw std::runtime_error("READY 输出失败，清理挂载");

    int received = 0;
    int error = sigwait(&signals, &received);
    if (error != 0)
      throw std::runtime_error("等待信号失败：" +
                               std::string(std::strerror(error)));

    bool cleanupFailed = false;

    try {
      attachment.detachProgram();
    } catch (const std::exception& error) {
      cleanupFailed = true;
      std::cerr << "XDP 卸载失败：" << error.what() << '\n';
    }

    if (options.profile == l4lb::xdp::XdpObjectProfile::kMapsV1) {
      try {
        auto packets = attachment.readPassPackets();
        std::cout << "XDP_STATS schema=1 pass_packets=" << packets << std::endl;
      } catch (const std::exception& error) {
        cleanupFailed = true;
        std::cerr << "XDP 统计失败：" << error.what() << '\n';
      }
    }

    if (options.profile == l4lb::xdp::XdpObjectProfile::kUdpDsrV2) {
      try {
        auto stats = attachment.readDsrStats();
        std::cout << "XDP_STATS schema=2 total_packets=" << stats.totalPackets
                  << " pass_packets=" << stats.passPackets
                  << " redirect_requests=" << stats.redirectRequests
                  << " drop_packets=" << stats.dropPackets
                  << " unsupported_packets=" << stats.unsupportedPackets
                  << " no_backend_packets=" << stats.noBackendPackets
                  << " invalid_config_packets=" << stats.invalidConfigPackets
                  << " helper_error_packets=" << stats.helperErrorPackets
                  << std::endl;
      } catch (const std::exception& error) {
        cleanupFailed = true;
        std::cerr << "XDP 统计失败：" << error.what() << '\n';
      }
    }

    if (cleanupFailed) return 1;

    std::cout << "DETACHED dev=" << options.device
              << " mode=" << l4lb::xdp::xdpAttachModeName(options.mode)
              << " prog_id=" << attachment.programId() << std::endl;
    return std::cout && received != SIGPIPE ? 0 : 1;
  } catch (const std::invalid_argument& error) {
    std::cerr << "参数错误：" << error.what() << '\n';
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "XDP 失败 dev=" << options.device
              << " mode=" << l4lb::xdp::xdpAttachModeName(options.mode) << "："
              << error.what() << '\n';
    return 1;
  }
}
