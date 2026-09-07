#include <iostream>
#include <string_view>

#include "config/config.h"

/** CLI 仅解释选项和展示配置模块结果。 */
int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "用法：l4lb --help | --check-config <path>\n"
                 "仅配置检查，尚不转发流量。路径相对于当前工作目录。\n";
    return 0;
  }
  if (argc != 3 || std::string_view(argv[1]) != "--check-config" ||
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
  std::cout << "配置有效：TCP，后端数量="
            << std::get<l4lb::Config>(result).backends.size() << '\n';
  return 0;
}
