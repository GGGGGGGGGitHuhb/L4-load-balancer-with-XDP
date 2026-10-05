#include "control/UdpService.h"

#include <iostream>
#include <stdexcept>

#include "control/MetricsService.h"
#include "net/UdpReactor.h"

namespace l4lb {
namespace {
std::string formatEndpoint(const Endpoint& endpoint) {
  return std::to_string(endpoint.address[0]) + "." +
         std::to_string(endpoint.address[1]) + "." +
         std::to_string(endpoint.address[2]) + "." +
         std::to_string(endpoint.address[3]) + ":" +
         std::to_string(endpoint.port);
}
}  // namespace

int runUdpService(const Config& config) {
  if (config.protocol != Protocol::kUdp)
    throw std::invalid_argument("UDP 入口仅支持 UDP 协议");

  std::unique_ptr<HealthSelection> selection;
  MetricsService metrics(config, selection);

  try {
    selection = std::make_unique<HealthSelection>(config);

    net::UdpReactorCallbacks callbacks;
    callbacks.select_backend = [&] { return selection->selectBackend(); };
    if (metrics.enabled())
      callbacks.statistics = [&](net::StatEvent statEvent) noexcept {
        metrics.recordStatEvent(statEvent);
      };
    if (selection->enabled() || metrics.enabled())
      callbacks.maintenance = [&] {
        if (selection->enabled()) selection->pollHealthProbes();
        metrics.emitPeriodicSnapshotIfDue();
      };
    callbacks.ready = [&] {
      std::cout << "UDP 服务已启动：" << formatEndpoint(config.listen)
                << std::endl;
      metrics.emitReadySnapshot();
    };
    callbacks.flow = [](const net::UdpFlowEvent& flowEvent) {
      std::cerr << "flow=" << flowEvent.id
                << " backend=" << formatEndpoint(flowEvent.backend)
                << " reason=" << flowEvent.reason;
      if (flowEvent.error) std::cerr << " errno=" << flowEvent.error;
      std::cerr << '\n';
    };
    callbacks.diagnostic = [](const std::string& reason, int error) {
      std::cerr << "UDP " << reason;
      if (error) std::cerr << " errno=" << error;
      std::cerr << '\n';
    };

    auto result = net::runUdpReactor(config.listen, callbacks);

    std::cerr << "UDP 服务已停止：全部 flow 已关闭（不保证在途数据排空）\n";
    metrics.finishMetrics(false);
    return result;
  } catch (...) {
    metrics.finishMetrics(true);
    std::cerr << "UDP 服务已停止：全部 flow 已关闭（不保证在途数据排空）\n";
    throw;
  }
}
}  // namespace l4lb
