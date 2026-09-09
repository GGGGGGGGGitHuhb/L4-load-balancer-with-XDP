#include "control/udp_service.h"

#include <iostream>
#include <stdexcept>

#include "control/metrics_service.h"
#include "net/udp_reactor.h"

namespace l4lb {
namespace {
std::string udp_endpoint_text(const Endpoint& e) {
  return std::to_string(e.address[0]) + "." + std::to_string(e.address[1]) +
         "." + std::to_string(e.address[2]) + "." +
         std::to_string(e.address[3]) + ":" + std::to_string(e.port);
}
}  // namespace

int run_udp_service(const Config& config) {
  if (config.protocol != Protocol::kUdp)
    throw std::invalid_argument("UDP 入口仅支持 UDP 协议");
  std::unique_ptr<HealthSelection> selection;
  MetricsService metrics(config, selection);
  try {
    selection = std::make_unique<HealthSelection>(config);
    net::UdpCallbacks callbacks;
    callbacks.select_backend = [&] { return selection->next(); };
    if (metrics.enabled())
      callbacks.statistics = [&](net::StatEvent e) noexcept {
        metrics.event(e);
      };
    if (selection->enabled() || metrics.enabled())
      callbacks.maintenance = [&] {
        if (selection->enabled()) selection->tick();
        metrics.maintenance();
      };
    callbacks.ready = [&] {
      std::cout << "UDP 服务已启动：" << udp_endpoint_text(config.listen)
                << std::endl;
      metrics.ready();
    };
    callbacks.flow = [](const net::UdpFlowEvent& e) {
      std::cerr << "flow=" << e.id
                << " backend=" << udp_endpoint_text(e.backend)
                << " reason=" << e.reason;
      if (e.error) std::cerr << " errno=" << e.error;
      std::cerr << '\n';
    };
    callbacks.diagnostic = [](const std::string& reason, int error) {
      std::cerr << "UDP " << reason;
      if (error) std::cerr << " errno=" << error;
      std::cerr << '\n';
    };
    auto result = net::run_udp(config.listen, callbacks);
    std::cerr << "UDP 服务已停止：全部 flow 已关闭（不保证在途数据排空）\n";
    metrics.finish(false);
    return result;
  } catch (...) {
    metrics.finish(true);
    std::cerr << "UDP 服务已停止：全部 flow 已关闭（不保证在途数据排空）\n";
    throw;
  }
}
}  // namespace l4lb
