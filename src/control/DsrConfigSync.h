#pragma once
#include <string>
#include <vector>

#include "xdp/DsrMapStore.h"

namespace l4lb::control {
struct DsrConfiguration {
  UdpDsrConfigValue config{};
  std::vector<UdpDsrBackendValue> backends;
};

/** Strict literal parsing and read-only inspection of existing interfaces. */
DsrConfiguration parseDsrConfiguration(const std::string& ingress,
                                       const std::string& vip,
                                       const std::vector<std::string>& targets);
/** Write all slots, read back the full image, and freeze before attach. */
void synchronizeDsrConfig(xdp::DsrMapAccess& maps,
                          const DsrConfiguration& configuration);
}  // namespace l4lb::control
