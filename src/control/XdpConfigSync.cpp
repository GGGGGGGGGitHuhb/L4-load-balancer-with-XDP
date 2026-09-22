#include "XdpConfigSync.h"

#include <arpa/inet.h>

#include <charconv>
#include <cstring>
#include <set>
#include <stdexcept>

namespace l4lb::control {
std::vector<XdpBackendValue> parseXdpBackends(
    const std::vector<std::string>& endpoints) {
  if (endpoints.size() > L4LB_XDP_MAX_BACKENDS)
    throw std::invalid_argument("后端最多 64 个");
  std::vector<XdpBackendValue> backends;
  std::set<std::pair<uint32_t, uint16_t>> seenEndpoints;
  for (const auto& endpoint : endpoints) {
    auto separator = endpoint.find(':');
    if (separator == std::string::npos)
      throw std::invalid_argument("后端必须为 IPv4:PORT：" + endpoint);
    std::string address = endpoint.substr(0, separator);
    std::string portText = endpoint.substr(separator + 1);
    XdpBackendValue backend{};
    unsigned port = 0;
    auto parsed = std::from_chars(portText.data(),
                                  portText.data() + portText.size(), port);
    if (inet_pton(AF_INET, address.c_str(), &backend.address) != 1 ||
        parsed.ec != std::errc{} ||
        parsed.ptr != portText.data() + portText.size() || port == 0 ||
        port > 65535)
      throw std::invalid_argument("无效 IPv4 后端：" + endpoint);
    uint32_t hostAddress = ntohl(backend.address);
    if (hostAddress == 0 || hostAddress == 0xffffffffU ||
        (hostAddress & 0xf0000000U) == 0xe0000000U)
      throw std::invalid_argument("后端必须为 IPv4 单播地址：" + endpoint);
    backend.port = htons(static_cast<uint16_t>(port));
    if (!seenEndpoints.emplace(backend.address, backend.port).second)
      throw std::invalid_argument("重复后端：" + endpoint);
    backends.push_back(backend);
  }
  return backends;
}

void synchronizeXdpConfig(xdp::ConfigMapAccess& maps,
                          const std::vector<XdpBackendValue>& backends) {
  if (backends.size() > L4LB_XDP_MAX_BACKENDS)
    throw std::invalid_argument("后端最多 64 个");
  for (uint32_t index = 0; index < L4LB_XDP_MAX_BACKENDS; ++index) {
    maps.writeBackend(
        index, index < backends.size() ? backends[index] : XdpBackendValue{});
  }
  XdpConfigValue config{L4LB_XDP_SCHEMA_VERSION,
                        static_cast<uint32_t>(backends.size())};
  maps.writeConfig(config);
  auto actualConfig = maps.readConfig();
  if (std::memcmp(&actualConfig, &config, sizeof(config)) != 0)
    throw std::runtime_error("cfg 回读不一致");
  for (uint32_t index = 0; index < L4LB_XDP_MAX_BACKENDS; ++index) {
    auto actual = maps.readBackend(index);
    auto expected =
        index < backends.size() ? backends[index] : XdpBackendValue{};
    if (std::memcmp(&actual, &expected, sizeof(expected)) != 0)
      throw std::runtime_error("backend 回读不一致 index=" +
                               std::to_string(index));
  }
  maps.freezeBackends();
  maps.freezeConfig();
}
}  // namespace l4lb::control
