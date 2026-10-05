#include "RuntimeDsrConfig.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "DsrConfigSync.h"
#include "XdpConfigSync.h"
#include "net/Fd.h"

namespace l4lb::control {
namespace {
constexpr size_t kMaxFileBytes = 64 * 1024;

bool validIdCharacter(unsigned char character) {
  return (character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z') ||
         (character >= '0' && character <= '9') || character == '_' ||
         character == '-';
}

bool validId(const std::string& id) {
  return !id.empty() && id.size() <= 32 &&
         std::all_of(id.begin(), id.end(), validIdCharacter);
}

void checkTargetLiteral(const RuntimeTargetText& target) {
  if (!validId(target.id)) throw std::invalid_argument("无效 target ID");
  if (target.egress.empty() || target.egress.size() >= 16 ||
      target.egress.find_first_of("/ \t\r\n:@") != std::string::npos)
    throw std::invalid_argument("无效 target 出口");
  if (target.destinationMac.size() != 17)
    throw std::invalid_argument("无效 target MAC");
  unsigned combined = 0;
  for (size_t index = 0; index < 6; ++index) {
    unsigned byte = 0;
    const char* begin = target.destinationMac.data() + 3 * index;
    const auto result = std::from_chars(begin, begin + 2, byte, 16);
    if (result.ec != std::errc{} || result.ptr != begin + 2 ||
        (index < 5 && begin[2] != ':') || (index == 0 && (byte & 1)))
      throw std::invalid_argument("target MAC 必须为单播");
    combined |= byte;
  }
  if (!combined) throw std::invalid_argument("target MAC 不能全零");
  parseXdpBackends({target.probeEndpoint});
}

bool sameFileVersion(const struct stat& before, const struct stat& after) {
  return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
         before.st_size == after.st_size &&
         before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
         before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
         before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
         before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}
}  // namespace

std::vector<RuntimeTargetText> parseRuntimeConfigText(std::string_view text) {
  if (text.size() > kMaxFileBytes)
    throw std::invalid_argument("runtime 配置超过64KiB");
  for (unsigned char character : text)
    if (character == 0 || character > 127 ||
        (character < 32 && character != '\n' && character != '\r' &&
         character != '\t'))
      throw std::invalid_argument("runtime 配置必须为有效 ASCII 文本");

  std::vector<RuntimeTargetText> targets;
  std::set<std::string> ids;
  bool schemaSeen = false;
  size_t lines = 0;
  std::istringstream input{std::string(text)};
  std::string line;
  while (std::getline(input, line)) {
    if (++lines > 256) throw std::invalid_argument("runtime 配置超过256行");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() > 1024 || line.find('\r') != std::string::npos)
      throw std::invalid_argument("runtime 配置行过长或含裸CR");
    const auto begin = line.find_first_not_of(" \t");
    if (begin == std::string::npos || line[begin] == '#') continue;
    std::istringstream fields(line.substr(begin));
    std::string first, link, probe, extra;
    fields >> first;
    if (first == "schema=1") {
      if (schemaSeen || (fields >> extra))
        throw std::invalid_argument("schema 重复或存在额外字段");
      schemaSeen = true;
      continue;
    }
    if (!first.starts_with("target=") || !(fields >> link >> probe) ||
        (fields >> extra))
      throw std::invalid_argument("未知或不完整 runtime 配置行");
    const auto separator = link.find('@');
    if (separator == std::string::npos)
      throw std::invalid_argument("target 缺少 EGRESS@MAC");
    RuntimeTargetText target{first.substr(7), link.substr(0, separator),
                             link.substr(separator + 1), probe};
    checkTargetLiteral(target);
    if (!ids.insert(target.id).second)
      throw std::invalid_argument("重复 target ID");
    targets.push_back(std::move(target));
    if (targets.size() > 64) throw std::invalid_argument("target 超过64个");
  }
  if (!schemaSeen) throw std::invalid_argument("缺少 schema=1");
  return targets;
}

std::string readRuntimeConfigFile(const std::string& path) {
  net::Fd file(
      open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW));
  if (file.fd() < 0)
    throw std::invalid_argument("无法打开 runtime 配置：" +
                                std::string(std::strerror(errno)));

  struct stat before {
  }, after{};

  if (fstat(file.fd(), &before) || !S_ISREG(before.st_mode) ||
      before.st_size < 0 || before.st_size > static_cast<off_t>(kMaxFileBytes))
    throw std::invalid_argument("runtime 配置必须为不超过64KiB的普通文件");
  std::string bytes;
  char buffer[4096];
  for (;;) {
    const auto count = read(file.fd(), buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) throw std::invalid_argument("读取 runtime 配置失败");
    if (!count) break;
    bytes.append(buffer, static_cast<size_t>(count));
    if (bytes.size() > kMaxFileBytes)
      throw std::invalid_argument("runtime 配置读取时超过64KiB");
  }
  if (fstat(file.fd(), &after) || !sameFileVersion(before, after) ||
      bytes.size() != static_cast<size_t>(before.st_size))
    throw std::invalid_argument("runtime 配置读取期间变化，请原子替换后重试");
  return bytes;
}

RuntimeDsrConfiguration loadRuntimeConfig(const std::string& path,
                                          const std::string& ingress,
                                          const std::string& vip) {
  const auto entries = parseRuntimeConfigText(readRuntimeConfigFile(path));
  std::vector<std::string> links;
  for (const auto& entry : entries)
    links.push_back(entry.egress + '@' + entry.destinationMac);
  const auto dsr = parseDsrConfiguration(ingress, vip, links);
  RuntimeDsrConfiguration result{dsr.config.vipAddress, dsr.config.vipPort, {}};
  std::set<std::tuple<uint32_t, uint32_t, uint16_t>> probes;
  for (size_t index = 0; index < entries.size(); ++index) {
    const auto endpoint =
        parseXdpBackends({entries[index].probeEndpoint}).front();
    if (endpoint.address == result.vipAddress)
      throw std::invalid_argument("probe 必须使用独立地址，不能是共享VIP");
    const auto& backend = dsr.backends[index];
    if (!probes.emplace(backend.ifindex, endpoint.address, endpoint.port)
             .second)
      throw std::invalid_argument("重复出口探测端点");
    result.targets.push_back({entries[index].id, entries[index].egress, backend,
                              endpoint.address, endpoint.port});
  }
  return result;
}

bool sameRuntimeTarget(const RuntimeTarget& left, const RuntimeTarget& right) {
  return left.id == right.id &&
         std::memcmp(&left.backend, &right.backend, sizeof(left.backend)) ==
             0 &&
         left.probeAddress == right.probeAddress &&
         left.probePort == right.probePort;
}

bool sameRuntimeConfig(const RuntimeDsrConfiguration& left,
                       const RuntimeDsrConfiguration& right) {
  return left.vipAddress == right.vipAddress && left.vipPort == right.vipPort &&
         left.targets.size() == right.targets.size() &&
         std::equal(left.targets.begin(), left.targets.end(),
                    right.targets.begin(), sameRuntimeTarget);
}
}  // namespace l4lb::control
