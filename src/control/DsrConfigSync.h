#pragma once
#include <string>
#include <vector>

#include "xdp/DsrMapStore.h"

namespace l4lb::control {
struct DsrConfiguration {
  UdpDsrConfigValue config{};

  std::vector<UdpDsrBackendValue> backends;
};

/** 严格解析字面值，并以只读方式检查现有接口。 */
DsrConfiguration parseDsrConfiguration(const std::string& ingress,
                                       const std::string& vip,
                                       const std::vector<std::string>& targets);

/** 写入所有槽并回读完整映像，在挂载之前冻结。 */
void synchronizeDsrConfig(xdp::DsrMapAccess& maps,
                          const DsrConfiguration& configuration);
}  // namespace l4lb::control
