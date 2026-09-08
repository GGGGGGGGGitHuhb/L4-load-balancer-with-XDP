#include "control/service.h"

#include <stdexcept>

#include "control/tcp_service.h"
#include "control/udp_service.h"
namespace l4lb {
int run_service(const Config& config) {
  switch (config.protocol) {
    case Protocol::kTcp:
      return run_tcp_service(config);
    case Protocol::kUdp:
      return run_udp_service(config);
  }
  throw std::invalid_argument("未知服务协议");
}
}  // namespace l4lb
