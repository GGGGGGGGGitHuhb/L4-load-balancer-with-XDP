#pragma once
#include <string>
#include <vector>

#include "xdp/MapStore.h"

namespace l4lb::control {
/** 打开或装载内核对象之前校验所有端点。 */
std::vector<XdpBackendValue> parseXdpBackends(
    const std::vector<std::string>& endpoints);

/** 完整写入并回读后才发布，然后冻结两个配置 map。 */
void synchronizeXdpConfig(xdp::ConfigMapAccess& maps,
                          const std::vector<XdpBackendValue>& backends);
}  // namespace l4lb::control
