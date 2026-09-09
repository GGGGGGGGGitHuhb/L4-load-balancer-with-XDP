#include <exception>
#include <iostream>
#include <string_view>

#include "config/config.h"
#include "control/service.h"

/** CLI 仅解释选项和展示配置模块结果。 */
int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "用法：l4lb --help | --check-config <path> | --run <path>\n"
                 "TCP 代理：固定轮询，无失败重试。UDP 按 flow 固定后端，尽力转发。"
                 "TCP/UDP 均可运行。路径相对于当前工作目录。\n"
                 "health_check 默认 off；tcp_connect 仅检查 TCP 握手。Unknown 预热期拒绝新会话/flow。\n"
                 "UDP 启用需在相同 IP/端口提供代表 UDP 服务的 TCP 健康端点；既有 flow 不迁移。\n";
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
