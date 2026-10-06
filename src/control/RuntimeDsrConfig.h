#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xdp/UdpDsrSchema.h"

namespace l4lb::control {
/** 尚未解析的文件条目：纯解析期间不访问接口或 socket。 */
struct RuntimeTargetText {
  std::string id;
  std::string egress;
  std::string destinationMac;
  std::string probeEndpoint;
};

struct RuntimeTarget {
  std::string id;
  std::string egress;
  UdpDsrBackendValue backend{};

  uint32_t probeAddress = 0;
  uint16_t probePort = 0;
};

struct RuntimeDsrConfiguration {
  uint32_t vipAddress = 0;
  uint16_t vipPort = 0;

  std::vector<RuntimeTarget> targets;
};

std::vector<RuntimeTargetText> parseRuntimeConfigText(std::string_view text);

std::string readRuntimeConfigFile(const std::string& path);

RuntimeDsrConfiguration loadRuntimeConfig(const std::string& path,
                                          const std::string& ingress,
                                          const std::string& vip);

bool sameRuntimeTarget(const RuntimeTarget& left, const RuntimeTarget& right);
bool sameRuntimeConfig(const RuntimeDsrConfiguration& left,
                       const RuntimeDsrConfiguration& right);

}  // namespace l4lb::control
