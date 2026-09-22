#include "DsrConfigSync.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <cstring>
#include <set>
#include <stdexcept>

#include "XdpConfigSync.h"

namespace l4lb::control {
namespace {
bool validMac(const unsigned char* mac) {
  return !(mac[0] & 1) && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
}

struct InterfaceInfo {
  uint32_t index;
  std::array<unsigned char, 6> mac;
};

InterfaceInfo inspectInterface(const std::string& name) {
  if (name.empty() || name.size() >= IF_NAMESIZE ||
      name.find_first_of("/ \t\r\n:@") != std::string::npos)
    throw std::invalid_argument("无效接口名称：" + name);

  struct Socket {
    int fd;

    ~Socket() {
      if (fd >= 0) close(fd);
    }
  } socketFd{socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)};

  if (socketFd.fd < 0) throw std::runtime_error("无法打开接口查询 socket");
  ifreq request{};
  std::memcpy(request.ifr_name, name.c_str(), name.size() + 1);
  if (ioctl(socketFd.fd, SIOCGIFINDEX, &request))
    throw std::invalid_argument("接口不存在：" + name);
  InterfaceInfo result{static_cast<uint32_t>(request.ifr_ifindex), {}};
  if (ioctl(socketFd.fd, SIOCGIFFLAGS, &request) ||
      !(request.ifr_flags & IFF_UP))
    throw std::invalid_argument("接口必须 UP：" + name);
  if (ioctl(socketFd.fd, SIOCGIFMTU, &request) || request.ifr_mtu < 1500)
    throw std::invalid_argument("接口 MTU 必须至少1500：" + name);
  if (ioctl(socketFd.fd, SIOCGIFHWADDR, &request) ||
      request.ifr_hwaddr.sa_family != ARPHRD_ETHER)
    throw std::invalid_argument("接口必须为 Ethernet：" + name);
  std::memcpy(result.mac.data(), request.ifr_hwaddr.sa_data, 6);
  if (!validMac(result.mac.data()))
    throw std::invalid_argument("接口 MAC 无效：" + name);
  return result;
}

std::array<unsigned char, 6> parseMac(const std::string& text) {
  std::array<unsigned char, 6> mac{};
  if (text.size() != 17)
    throw std::invalid_argument("MAC 必须为六组两位十六进制");
  for (size_t i = 0; i < 6; ++i) {
    unsigned byte = 0;
    const char* begin = text.data() + 3 * i;
    auto parsed = std::from_chars(begin, begin + 2, byte, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != begin + 2 ||
        (i < 5 && begin[2] != ':'))
      throw std::invalid_argument("无效 MAC：" + text);
    mac[i] = static_cast<unsigned char>(byte);
  }
  if (!validMac(mac.data()))
    throw std::invalid_argument("MAC 必须为非零单播：" + text);
  return mac;
}
}  // namespace

DsrConfiguration parseDsrConfiguration(
    const std::string& ingress, const std::string& vip,
    const std::vector<std::string>& targets) {
  auto endpoint = parseXdpBackends({vip}).front();
  if (targets.size() > L4LB_DSR_MAX_BACKENDS)
    throw std::invalid_argument("后端最多 64 个");
  // Parse every literal before inspecting interfaces or opening an object.
  std::vector<std::pair<std::string, std::array<unsigned char, 6>>>
      parsedTargets;
  for (const auto& target : targets) {
    auto separator = target.find('@');
    if (separator == std::string::npos)
      throw std::invalid_argument("target 必须为 EGRESS@MAC");
    parsedTargets.emplace_back(target.substr(0, separator),
                               parseMac(target.substr(separator + 1)));
  }
  auto input = inspectInterface(ingress);
  DsrConfiguration result{
      {L4LB_DSR_SCHEMA_VERSION, static_cast<uint32_t>(targets.size()),
       endpoint.address, endpoint.port, 0},
      {}};
  std::set<std::pair<uint32_t, std::array<unsigned char, 6>>> seen;
  for (const auto& [name, mac] : parsedTargets) {
    auto output = inspectInterface(name);
    if (output.index == input.index)
      throw std::invalid_argument("出口不能等于入口");
    if (!seen.emplace(output.index, mac).second)
      throw std::invalid_argument("重复 target");
    UdpDsrBackendValue backend{};
    backend.ifindex = output.index;
    std::memcpy(backend.destinationMac, mac.data(), 6);
    std::memcpy(backend.sourceMac, output.mac.data(), 6);
    result.backends.push_back(backend);
  }
  return result;
}

void synchronizeDsrConfig(xdp::DsrMapAccess& maps,
                          const DsrConfiguration& configuration) {
  const auto& backends = configuration.backends;
  if (backends.size() > L4LB_DSR_MAX_BACKENDS)
    throw std::invalid_argument("后端最多 64 个");
  const auto& config = configuration.config;
  uint32_t address = ntohl(config.vipAddress);
  if (config.schemaVersion != L4LB_DSR_SCHEMA_VERSION ||
      config.backendCount != backends.size() || config.reserved ||
      !config.vipPort || !address || address == 0xffffffffU ||
      (address & 0xf0000000U) == 0xe0000000U)
    throw std::invalid_argument("无效 DSR 配置");
  for (const auto& backend : backends)
    if (!backend.ifindex || !validMac(backend.destinationMac) ||
        !validMac(backend.sourceMac))
      throw std::invalid_argument("无效 DSR backend");
  for (uint32_t index = 0; index < L4LB_DSR_MAX_BACKENDS; ++index) {
    maps.writeBackend(index, index < backends.size() ? backends[index]
                                                     : UdpDsrBackendValue{});
  }
  maps.writeConfig(config);
  auto actualConfig = maps.readConfig();
  if (std::memcmp(&actualConfig, &config, sizeof(config)) != 0)
    throw std::runtime_error("cfg 回读不一致");
  for (uint32_t index = 0; index < L4LB_DSR_MAX_BACKENDS; ++index) {
    auto actual = maps.readBackend(index);
    auto expected =
        index < backends.size() ? backends[index] : UdpDsrBackendValue{};
    if (std::memcmp(&actual, &expected, sizeof(expected)) != 0)
      throw std::runtime_error("backend 回读不一致 index=" +
                               std::to_string(index));
  }
  maps.freezeBackends();
  maps.freezeConfig();
}
}  // namespace l4lb::control
