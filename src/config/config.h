#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace l4lb {
/** 数字 IPv4 端点，不表示可达性。 */
struct Endpoint {
  std::array<std::uint8_t, 4> address{};
  std::uint16_t port{};
  bool operator==(const Endpoint&) const = default;
};
enum class Protocol { kTcp, kUdp };
enum class SchedulerKind { kRoundRobin };
/** 仅完整校验成功后返回；后端按输入顺序保存。 */
struct Config {
  Endpoint listen;
  std::vector<Endpoint> backends;
  Protocol protocol = Protocol::kTcp;
  SchedulerKind scheduler = SchedulerKind::kRoundRobin;
};
enum class ErrorKind { kFile, kSyntax, kField, kMissing };
/** 文件错误 line=0；缺失字段 eof=true，其余错误定位实际行。 */
struct ConfigError {
  ErrorKind kind;
  std::size_t line;
  bool eof;
  std::string message;
};
using ConfigResult = std::variant<Config, ConfigError>;
inline constexpr std::size_t kMaxConfigBytes = 65536;
/** 纯解析，无打印或 I/O；失败不返回部分配置。 */
ConfigResult parse_config(std::string_view text);
/** 只读普通文件（允许符号链接），最多读取上限加一字节以检测增长。 */
ConfigResult load_config(const std::string& path);
}  // namespace l4lb
