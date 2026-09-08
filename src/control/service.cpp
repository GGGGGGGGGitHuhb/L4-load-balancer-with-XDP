#include "control/service.h"

#include <stdexcept>

#include "control/tcp_service.h"
namespace l4lb {
int run_service(const Config& config) {
  switch (config.protocol) {
    case Protocol::kTcp:
      return run_tcp_service(config);
    case Protocol::kUdp:
      throw std::invalid_argument("当前阶段尚不支持 UDP 转发");
  }
  throw std::invalid_argument("未知服务协议");
}
}  // namespace l4lb
