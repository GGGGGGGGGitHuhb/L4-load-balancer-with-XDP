#include <exception>
#include <iostream>
#include <string_view>

#include "config/config.h"
#include "control/service.h"

/** CLI 仅解释选项和展示配置模块结果。 */
int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout
        << "用法：l4lb --help | --check-config <path> | --run <path>\n"
           "TCP 代理：固定轮询，无失败重试。UDP 按 flow 固定后端，尽力转发。"
           "TCP/UDP 均可运行。路径相对于当前工作目录。\n"
           "health_check 默认 off；tcp_connect 仅检查 TCP 握手。Unknown "
           "预热期拒绝新会话/flow。\n"
           "metrics 默认 off，可选 stderr（单行 metrics JSON，独立于 "
           "health_check）；同步 stderr 可能阻塞业务。\n"
           "UDP 仅在启用 health_check=tcp_connect 时需在相同 IP/端口提供代表 "
           "UDP 服务的 TCP 健康端点；既有 "
           "flow 不迁移。\n"
           "ready 仅表示监听就绪，不保证后端可达或 Healthy。\n"
           "TCP 停止仅尝试在固定 1 秒内排空用户态 pending，不保证送达；"
           "UDP 停止关闭 "
           "flows，不保证排空。同步输出阻塞时不保证进程按时退出。\n";
    return 0;
  }
  if (argc != 3 ||
      (std::string_view(argv[1]) != "--check-config" &&
       std::string_view(argv[1]) != "--run") ||
      std::string_view(argv[2]).empty() ||
      std::string_view(argv[2]).starts_with("--")) {
    std::cerr << "用法错误：请使用 l4lb --help 查看帮助\n";
    return 2;
  }
  const auto result = l4lb::load_config(argv[2]);
  if (const auto* error = std::get_if<l4lb::ConfigError>(&result)) {
    std::cerr << "配置错误：";
    if (error->eof)
      std::cerr << "EOF（第 " << error->line << " 行）：";
    else if (error->line != 0)
      std::cerr << "第 " << error->line << " 行：";
    std::cerr << error->message << '\n';
    return 1;
  }
  if (std::string_view(argv[1]) == "--run") {
    try {
      return l4lb::run_service(std::get<l4lb::Config>(result));
    } catch (const std::exception& error) {
      std::cerr << "服务错误：" << error.what() << '\n';
      return 1;
    }
  }
  std::cout << "配置有效："
            << (std::get<l4lb::Config>(result).protocol == l4lb::Protocol::kTcp
                    ? "TCP"
                    : "UDP")
            << "，后端数量=" << std::get<l4lb::Config>(result).backends.size()
            << '\n';
  return 0;
}
