#include "control/Service.h"

#include <stdexcept>

#include "control/TcpService.h"
#include "control/UdpService.h"

namespace l4lb {
int runConfiguredService(const Config& config) {
  switch (config.protocol) {
    case Protocol::kTcp:
      return runTcpService(config);
    case Protocol::kUdp:
      return runUdpService(config);
  }
  throw std::invalid_argument("未知服务协议");
}
}  // namespace l4lb
