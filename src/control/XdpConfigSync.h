#pragma once
#include <string>
#include <vector>

#include "xdp/MapStore.h"

namespace l4lb::control {
/** Validate all endpoints before opening/loading a kernel object. */
std::vector<XdpBackendValue> parseXdpBackends(
    const std::vector<std::string>& endpoints);
/** Publish only after complete write/readback, then freeze both config maps. */
void synchronizeXdpConfig(xdp::ConfigMapAccess& maps,
                          const std::vector<XdpBackendValue>& backends);
}  // namespace l4lb::control
