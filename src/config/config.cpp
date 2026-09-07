#include "config/config.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>

namespace l4lb {
namespace {
std::string_view trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string_view::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}
std::optional<unsigned> number(std::string_view value, unsigned maximum) {
  if (value.empty() || (value.size() > 1 && value.front() == '0')) return {};
  unsigned result = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') return {};
    const unsigned digit = static_cast<unsigned>(c - '0');
    if (result > (maximum - digit) / 10) return {};
    result = result * 10 + digit;
  }
  if (result > maximum) return {};
  return result;
}
std::optional<Endpoint> endpoint(std::string_view value, bool backend) {
  const auto colon = value.find(':');
  if (colon == std::string_view::npos) return {};
  const auto port = number(value.substr(colon + 1), 65535);
  if (!port || *port == 0) return {};
  auto address = value.substr(0, colon);
  Endpoint result;
  result.port = static_cast<std::uint16_t>(*port);
  for (std::size_t i = 0; i < 4; ++i) {
    const auto dot = address.find('.');
    if ((i < 3) != (dot != std::string_view::npos)) return {};
    const auto part = number(address.substr(0, dot), 255);
    if (!part) return {};
    result.address[i] = static_cast<std::uint8_t>(*part);
    if (i < 3) address.remove_prefix(dot + 1);
  }
  const auto& a = result.address;
  if ((a[0] >= 224 && a[0] <= 239) ||
      a == std::array<std::uint8_t, 4>{255, 255, 255, 255} ||
      (backend && a == std::array<std::uint8_t, 4>{0, 0, 0, 0}))
    return {};
  return result;
}
/** 描述符单一所有者；即使字符串分配抛异常也会关闭。 */
class File {
 public:
  explicit File(int fd) : fd_(fd) {}
  ~File() {
    if (fd_ >= 0) close(fd_);
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  int get() const { return fd_; }

 private:
  int fd_;
};
}  // namespace

ConfigResult parse_config(std::string_view text) {
  if (text.size() > kMaxConfigBytes)
    return ConfigError{ErrorKind::kFile, 0, false, "配置超过 65536 字节"};
  Config config;
  bool has_listen = false;
  std::size_t line_number = 0;
  while (!text.empty()) {
    ++line_number;
    const auto newline = text.find('\n');
    auto line = text.substr(0, newline);
    if (newline != std::string_view::npos) {
      text.remove_prefix(newline + 1);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    } else {
      text = {};
    }
    auto fail = [&](ErrorKind kind, const char* message) -> ConfigResult {
      return ConfigError{kind, line_number, false, message};
    };
    // 逐行检查，避免后面的编码问题覆盖前面的首个错误。
    if (line.find('\0') != std::string_view::npos ||
        line.find('\r') != std::string_view::npos ||
        line.find("\xEF\xBB\xBF") != std::string_view::npos)
      return fail(ErrorKind::kSyntax, "不允许 NUL、BOM 或孤立 CR");
    line = trim(line);
    if (line.empty() || line.front() == '#') continue;
    const auto equal = line.find('=');
    if (equal == std::string_view::npos ||
        line.find('=', equal + 1) != std::string_view::npos)
      return fail(ErrorKind::kSyntax, "每个字段必须恰有一个等号");
    const auto key = trim(line.substr(0, equal));
    const auto value = trim(line.substr(equal + 1));
    if (key.empty() || value.empty())
      return fail(ErrorKind::kSyntax, "键和值不能为空");
    if (key != "listen" && key != "backend")
      return fail(ErrorKind::kField, "未知配置键");
    if (key == "listen" && has_listen)
      return fail(ErrorKind::kField, "listen 不能重复");
    const auto parsed = endpoint(value, key == "backend");
    if (!parsed) return fail(ErrorKind::kField, "无效的 IPv4 地址或端口");
    if (key == "listen") {
      config.listen = *parsed;
      has_listen = true;
    } else {
      if (config.backends.size() == 256)
        return fail(ErrorKind::kField, "backend 最多 256 个");
      if (std::find(config.backends.begin(), config.backends.end(), *parsed) !=
          config.backends.end())
        return fail(ErrorKind::kField, "backend 端点不能重复");
      config.backends.push_back(*parsed);
    }
  }
  if (!has_listen || config.backends.empty())
    return ConfigError{ErrorKind::kMissing, line_number + 1, true,
                       "必须包含一个 listen 和至少一个 backend"};
  return config;
}

ConfigResult load_config(const std::string& path) {
  auto fail = [&](const std::string& reason) -> ConfigResult {
    return ConfigError{ErrorKind::kFile, 0, false,
                       "文件 " + path + "：" + reason};
  };
  // 非阻塞打开可使 FIFO 在无写端时立即返回，之后按已打开对象检查类型。
  File file(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
  if (file.get() < 0) return fail(std::strerror(errno));
  struct stat info {};
  if (fstat(file.get(), &info) != 0) return fail(std::strerror(errno));
  if (!S_ISREG(info.st_mode)) return fail("必须为普通文件");
  std::string contents;
  std::array<char, 4096> buffer{};
  while (contents.size() <= kMaxConfigBytes) {
    const auto limit =
        std::min(buffer.size(), kMaxConfigBytes + 1 - contents.size());
    const auto count = read(file.get(), buffer.data(), limit);
    if (count < 0) return fail(std::strerror(errno));
    if (count == 0) return parse_config(contents);
    contents.append(buffer.data(), static_cast<std::size_t>(count));
  }
  return fail("配置超过 65536 字节");
}
}  // namespace l4lb
