#include "config/Config.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>

namespace l4lb {
namespace {
ConfigResult makeConfigLineError(ErrorKind kind, std::size_t lineNumber,
                                 const char* message) {
  return ConfigError{kind, lineNumber, false, message};
}

ConfigResult makeConfigFileError(const std::string& path,
                                 const std::string& reason) {
  return ConfigError{ErrorKind::kFile, 0, false,
                     "文件 " + path + "：" + reason};
}

std::string_view trimConfigWhitespace(std::string_view value) {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string_view::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

std::optional<unsigned> parseUnsignedDecimal(std::string_view value,
                                             unsigned maximum) {
  if (value.empty() || (value.size() > 1 && value.front() == '0')) return {};

  unsigned result = 0;
  for (const char character : value) {
    if (character < '0' || character > '9') return {};
    const unsigned digit = static_cast<unsigned>(character - '0');
    if (result > (maximum - digit) / 10) return {};
    result = result * 10 + digit;
  }

  if (result > maximum) return {};
  return result;
}

std::optional<Endpoint> parseIpv4Endpoint(std::string_view value,
                                          bool backend) {
  const auto colon = value.find(':');
  if (colon == std::string_view::npos) return {};
  const auto port = parseUnsignedDecimal(value.substr(colon + 1), 65535);
  if (!port || *port == 0) return {};

  auto address = value.substr(0, colon);
  Endpoint result;
  result.port = static_cast<std::uint16_t>(*port);
  for (std::size_t addressOctetIndex = 0; addressOctetIndex < 4;
       ++addressOctetIndex) {
    const auto dot = address.find('.');
    if ((addressOctetIndex < 3) != (dot != std::string_view::npos)) return {};
    const auto part = parseUnsignedDecimal(address.substr(0, dot), 255);
    if (!part) return {};
    result.address[addressOctetIndex] = static_cast<std::uint8_t>(*part);
    if (addressOctetIndex < 3) address.remove_prefix(dot + 1);
  }

  const auto& addressBytes = result.address;
  if ((addressBytes[0] >= 224 && addressBytes[0] <= 239) ||
      addressBytes == std::array<std::uint8_t, 4>{255, 255, 255, 255} ||
      (backend && addressBytes == std::array<std::uint8_t, 4>{0, 0, 0, 0}))
    return {};
  return result;
}

/** 描述符单一所有者；即使字符串分配抛异常也会关闭。 */
class ConfigFileFd {
 public:
  explicit ConfigFileFd(int fd) : fd_(fd) {}

  ~ConfigFileFd() {
    if (fd_ >= 0) close(fd_);
  }

  ConfigFileFd(const ConfigFileFd&) = delete;
  ConfigFileFd& operator=(const ConfigFileFd&) = delete;

  int fd() const { return fd_; }

 private:
  int fd_;
};
}  // namespace

ConfigResult parseConfig(std::string_view text) {
  if (text.size() > kMaxConfigBytes)
    return ConfigError{ErrorKind::kFile, 0, false, "配置超过 65536 字节"};

  Config config;
  bool hasListen = false;
  bool hasProtocol = false;
  bool hasScheduler = false;
  bool hasHealth = false;
  bool hasMetrics = false;
  std::size_t lineNumber = 0;

  while (!text.empty()) {
    ++lineNumber;
    const auto newline = text.find('\n');
    auto line = text.substr(0, newline);
    if (newline != std::string_view::npos) {
      text.remove_prefix(newline + 1);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    } else {
      text = {};
    }

    auto makeConfigError = [&](ErrorKind kind,
                               const char* message) -> ConfigResult {
      return makeConfigLineError(kind, lineNumber, message);
    };

    // 逐行检查，避免后面的编码问题覆盖前面的首个错误。
    if (line.find('\0') != std::string_view::npos ||
        line.find('\r') != std::string_view::npos ||
        line.find("\xEF\xBB\xBF") != std::string_view::npos)
      return makeConfigError(ErrorKind::kSyntax, "不允许 NUL、BOM 或孤立 CR");

    line = trimConfigWhitespace(line);
    if (line.empty() || line.front() == '#') continue;

    const auto equal = line.find('=');
    if (equal == std::string_view::npos ||
        line.find('=', equal + 1) != std::string_view::npos)
      return makeConfigError(ErrorKind::kSyntax, "每个字段必须恰有一个等号");
    const auto key = trimConfigWhitespace(line.substr(0, equal));
    const auto value = trimConfigWhitespace(line.substr(equal + 1));
    if (key.empty() || value.empty())
      return makeConfigError(ErrorKind::kSyntax, "键和值不能为空");

    if (key == "protocol") {
      if (hasProtocol)
        return makeConfigError(ErrorKind::kField, "protocol 不能重复");
      if (value == "tcp")
        config.protocol = Protocol::kTcp;
      else if (value == "udp")
        config.protocol = Protocol::kUdp;
      else
        return makeConfigError(ErrorKind::kField, "protocol 仅支持 tcp/udp");
      hasProtocol = true;
      continue;
    }

    if (key == "metrics") {
      if (hasMetrics)
        return makeConfigError(ErrorKind::kField, "metrics 不能重复");
      if (value == "off")
        config.metrics = MetricsKind::kOff;
      else if (value == "stderr")
        config.metrics = MetricsKind::kStderr;
      else
        return makeConfigError(ErrorKind::kField, "metrics 仅支持 off/stderr");
      hasMetrics = true;
      continue;
    }

    if (key == "health_check") {
      if (hasHealth)
        return makeConfigError(ErrorKind::kField, "health_check 不能重复");
      if (value == "off")
        config.healthCheck = HealthCheck::kOff;
      else if (value == "tcp_connect")
        config.healthCheck = HealthCheck::kTcpConnect;
      else
        return makeConfigError(ErrorKind::kField,
                               "health_check 仅支持 off/tcp_connect");
      hasHealth = true;
      continue;
    }

    if (key == "scheduler") {
      if (hasScheduler)
        return makeConfigError(ErrorKind::kField, "scheduler 不能重复");
      if (value != "round_robin")
        return makeConfigError(ErrorKind::kField,
                               "scheduler 仅支持 round_robin");
      config.scheduler = SchedulerKind::kRoundRobin;
      hasScheduler = true;
      continue;
    }

    if (key != "listen" && key != "backend")
      return makeConfigError(ErrorKind::kField, "未知配置键");
    if (key == "listen" && hasListen)
      return makeConfigError(ErrorKind::kField, "listen 不能重复");

    const auto parsed = parseIpv4Endpoint(value, key == "backend");
    if (!parsed)
      return makeConfigError(ErrorKind::kField, "无效的 IPv4 地址或端口");

    if (key == "listen") {
      config.listen = *parsed;
      hasListen = true;
    } else {
      if (config.backends.size() == 256)
        return makeConfigError(ErrorKind::kField, "backend 最多 256 个");
      if (std::find(config.backends.begin(), config.backends.end(), *parsed) !=
          config.backends.end())
        return makeConfigError(ErrorKind::kField, "backend 端点不能重复");
      config.backends.push_back(*parsed);
    }
  }

  if (!hasListen || config.backends.empty())
    return ConfigError{ErrorKind::kMissing, lineNumber + 1, true,
                       "必须包含一个 listen 和至少一个 backend"};
  return config;
}

ConfigResult loadConfig(const std::string& path) {
  auto makeConfigError = [&](const std::string& reason) -> ConfigResult {
    return makeConfigFileError(path, reason);
  };

  // 非阻塞打开可使 FIFO 在无写端时立即返回，之后按已打开对象检查类型。
  ConfigFileFd file(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
  if (file.fd() < 0) return makeConfigError(std::strerror(errno));

  struct stat info {};

  if (fstat(file.fd(), &info) != 0)
    return makeConfigError(std::strerror(errno));
  if (!S_ISREG(info.st_mode)) return makeConfigError("必须为普通文件");

  std::string contents;
  std::array<char, 4096> buffer{};
  while (contents.size() <= kMaxConfigBytes) {
    const auto limit =
        std::min(buffer.size(), kMaxConfigBytes + 1 - contents.size());
    const auto count = read(file.fd(), buffer.data(), limit);
    if (count < 0) return makeConfigError(std::strerror(errno));
    if (count == 0) return parseConfig(contents);
    contents.append(buffer.data(), static_cast<std::size_t>(count));
  }

  return makeConfigError("配置超过 65536 字节");
}
}  // namespace l4lb
