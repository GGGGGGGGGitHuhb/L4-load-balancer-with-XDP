#include "control/TcpService.h"

#include <iostream>
#include <stdexcept>
#include <system_error>

#include "control/MetricsService.h"
#include "net/TcpReactor.h"

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

int runTcpService(const Config& config) {
  if (config.protocol != Protocol::kTcp)
    throw std::invalid_argument("TCP 入口仅支持 TCP 协议");

  std::unique_ptr<HealthSelection> selection;
  MetricsService metrics(config, selection);

  try {
    selection = std::make_unique<HealthSelection>(config);

    net::TcpReactorCallbacks callbacks;
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
      std::cout << "TCP 服务已启动：" << formatEndpoint(config.listen)
                << std::endl;
      metrics.emitReadySnapshot();
    };
    callbacks.stopping = [](const net::StopEvent& stopEvent) {
      std::cerr << "TCP draining pending_c2b=" << stopEvent.pending[0]
                << " pending_b2c=" << stopEvent.pending[1]
                << " deadline_ns=" << stopEvent.deadlineNs << '\n';
    };
    callbacks.session = [](const net::SessionEvent& sessionEvent) {
      std::cerr << "session=" << sessionEvent.id
                << " backend=" << formatEndpoint(sessionEvent.backend)
                << " reason=" << sessionEvent.reason;
      if (!sessionEvent.accepted)
        std::cerr << " sent_c2b=" << sessionEvent.sent[0]
                  << " sent_b2c=" << sessionEvent.sent[1];
      if (sessionEvent.error)
        std::cerr << " errno=" << sessionEvent.error << " "
                  << std::generic_category().message(sessionEvent.error);
      std::cerr << '\n';
    };
    callbacks.diagnostic = [](const std::string& operation, int error) {
      std::cerr << operation << " errno=" << error << " "
                << std::generic_category().message(error) << '\n';
    };

    auto result = net::runTcpReactor(config.listen, callbacks);

    std::cerr
        << "TCP 服务已停止：已尝试有界排空用户态待发队列，不保证在途数据送达\n";
    metrics.finishMetrics(false);
    return result;
  } catch (...) {
    metrics.finishMetrics(true);
    throw;
  }
}
}  // namespace l4lb
