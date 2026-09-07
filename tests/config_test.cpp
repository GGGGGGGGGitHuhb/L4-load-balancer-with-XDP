#include "config/config.h"

#include <iostream>
#include <string>
#include <vector>

namespace {
int checks = 0;
int failures = 0;
void check(bool condition, const std::string& name) {
  ++checks;
  if (!condition) {
    ++failures;
    std::cerr << "失败：" << name << '\n';
  }
}
void invalid(const std::string& text, std::size_t line, bool eof,
             const std::string& name) {
  const auto result = l4lb::parse_config(text);
  const auto* error = std::get_if<l4lb::ConfigError>(&result);
  check(error != nullptr && error->line == line && error->eof == eof, name);
}
}  // namespace

int main(int argc, char**) {
  // Reviewer 可执行此模式证明 Release 中检查失败仍返回非零。
  if (argc > 1) {
    check(false, "故意失败验证");
    return failures ? 1 : 0;
  }
  const std::string listen = "listen=0.0.0.0:1\n";
  const std::string backend = "backend=127.0.0.1:65535\n";
  for (const std::string newline : {"\n", "\r\n"}) {
    for (bool final_newline : {false, true}) {
      const auto text = " \t# 中文注释" + newline + "\t" + newline +
                        " listen \t=\t0.0.0.0:1 " + newline +
                        " backend = 192.168.1.2:65535" + newline +
                        "backend=127.0.0.1:1" + (final_newline ? newline : "");
      const auto result = l4lb::parse_config(text);
      const auto* config = std::get_if<l4lb::Config>(&result);
      check(config && config->listen == l4lb::Endpoint{{0, 0, 0, 0}, 1} &&
                config->backends ==
                    std::vector<l4lb::Endpoint>{{{192, 168, 1, 2}, 65535},
                                                {{127, 0, 0, 1}, 1}},
            "换行、空白、端口边界与端点顺序");
    }
  }
  check(std::holds_alternative<l4lb::Config>(
            l4lb::parse_config(backend + listen)),
        "listen 可后置");
  invalid("", 1, true, "空文件");
  invalid(" # 只有注释\n", 2, true, "仅注释");
  invalid(listen, 2, true, "缺 backend");
  invalid(backend, 2, true, "缺 listen");
  invalid(listen + backend + listen, 3, false, "重复 listen");
  invalid(listen + backend + backend, 3, false, "重复 backend");
  const std::vector<std::string> bad_lines = {"unknown=1",
                                              "Listen=127.0.0.1:1",
                                              "=127.0.0.1:1",
                                              "listen=",
                                              "listen",
                                              "listen==127.0.0.1:1",
                                              "[section]",
                                              "监听=1",
                                              "listen=127.0.0.1:1 # inline",
                                              "listen=\"127.0.0.1:1\"",
                                              "listen=${HOST}:1",
                                              "include=other.conf",
                                              "listen=127.0.0.1\\:1",
                                              "listen=１２７.0.0.1:1",
                                              "listen=127.0.0.1:\t1\t2",
                                              "listen=127.0.0.1:1\v",
                                              "listen=127.0.0.1:1\f"};
  for (const auto& line : bad_lines)
    invalid(line + "\n" + backend, 1, false, "非法字段 " + line);
  const std::vector<std::string> bad_endpoints = {
      "127.0.0.1:0",
      "127.0.0.1:65536",
      "127.0.0.1:+1",
      "127.0.0.1:-1",
      "127.0.0.1:01",
      "127.0.0.1:1x",
      "127.0.0.1:99999999999999999999",
      "256.0.0.1:1",
      "01.0.0.1:1",
      "00.0.0.1:1",
      "-1.0.0.1:1",
      "+1.0.0.1:1",
      "1.2.3:1",
      "1.2.3.4.5:1",
      ".1.2.3:1",
      "1..2.3:1",
      "1.2.3.:1",
      "127.0.0.1",
      "127.0.0.1:",
      "127.0.0.1:1:2",
      "[::1]:1",
      "::1:1",
      "localhost:1",
      "127.0. 0.1:1",
      "127.0.0.1 :1",
      "127.0.0.1: 1",
      "224.0.0.0:1",
      "239.255.255.255:1",
      "255.255.255.255:1"};
  for (const auto& ep : bad_endpoints) {
    invalid("listen=" + ep + "\n" + backend, 1, false, "非法 listen " + ep);
    invalid(listen + "backend=" + ep, 2, false, "非法 backend " + ep);
  }
  invalid(listen + "backend=0.0.0.0:1", 2, false, "backend 通配地址");
  for (const auto& ep :
       {"223.255.255.255:1", "240.0.0.0:1", "255.255.255.254:1"}) {
    check(std::holds_alternative<l4lb::Config>(
              l4lb::parse_config(listen + "backend=" + ep)),
          "多播和广播相邻边界");
  }
  invalid("\xEF\xBB\xBF" + listen + backend, 1, false, "UTF-8 BOM");
  invalid(listen + std::string("# null\0comment\n", 15) + backend, 2, false,
          "注释 NUL");
  invalid(std::string("listen=127.0.0.1:1\0\n", 20) + backend, 1, false,
          "字段 NUL");
  invalid(listen + backend + "# comment\r", 3, false, "末尾孤立 CR");
  invalid(listen + "\rbackend=127.0.0.1:2\n", 2, false, "行中孤立 CR");
  invalid("bad\n\xEF\xBB\xBF", 1, false, "首错优先于后续 BOM");
  invalid("bad\n" + std::string("\0", 1), 1, false, "首错优先于后续 NUL");
  std::string many = listen;
  for (int i = 1; i <= 256; ++i)
    many += "backend=127.0.0.1:" + std::to_string(i) + "\n";
  const auto result = l4lb::parse_config(many);
  const auto* config = std::get_if<l4lb::Config>(&result);
  check(config && config->backends.size() == 256, "256 个后端");
  if (config)
    for (std::size_t i = 0; i < config->backends.size(); ++i)
      check(config->backends[i].port == i + 1, "全部后端顺序");
  invalid(many + "backend=127.0.0.1:257", 258, false, "257 个后端");
  auto bounded = listen + backend + "#";
  bounded.resize(65536, 'x');
  check(std::holds_alternative<l4lb::Config>(l4lb::parse_config(bounded)),
        "65536 字节");
  invalid(bounded + "x", 0, false, "65537 字节");
  std::cout << "检查数=" << checks << "，失败数=" << failures << '\n';
  return failures ? 1 : 0;
}
