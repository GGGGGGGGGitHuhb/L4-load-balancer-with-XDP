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

class UdpServiceRuntime {
 public:
  explicit UdpServiceRuntime(const Config& config)
      : config_(config), metrics_(config, selection_) {}

  int runUdpServiceLifecycle() {
    try {
      selection_ = std::make_unique<HealthSelection>(config_);

      net::UdpReactorCallbacks callbacks;
      bindReactorCallbacks(callbacks);

      auto result = net::runUdpReactor(config_.listen, callbacks);

      std::cerr << "UDP 服务已停止：全部 flow 已关闭（不保证在途数据排空）\n";
      metrics_.finishMetrics(false);
      return result;
    } catch (...) {
      metrics_.finishMetrics(true);
      std::cerr << "UDP 服务已停止：全部 flow 已关闭（不保证在途数据排空）\n";
      throw;
    }
  }

 private:
  void bindReactorCallbacks(net::UdpReactorCallbacks& callbacks) {
    callbacks.setBackendSelector([this]() { return selectBackend(); });

    if (metrics_.enabled())
      callbacks.setStatisticsCallback(
          [this](net::StatEvent statEvent) noexcept {
            onStatistics(statEvent);
          });
    if (selection_->enabled() || metrics_.enabled())
      callbacks.setMaintenanceCallback([this]() { onMaintenance(); });

    callbacks.setReadyCallback([this]() { onReady(); });
    callbacks.setFlowCallback(
        [this](const net::UdpFlowEvent& flowEvent) { onFlow(flowEvent); });
    callbacks.setDiagnosticCallback(
        [this](const std::string& reason, int error) {
          onDiagnostic(reason, error);
        });
  }

  std::optional<Endpoint> selectBackend() {
    return selection_->selectBackend();
  }

  void onStatistics(net::StatEvent statEvent) noexcept {
    metrics_.recordStatEvent(statEvent);
  }

  void onMaintenance() {
    if (selection_->enabled()) selection_->pollHealthProbes();

    metrics_.emitPeriodicSnapshotIfDue();
  }

  void onReady() {
    std::cout << "UDP 服务已启动：" << formatEndpoint(config_.listen)
              << std::endl;

    metrics_.emitReadySnapshot();
  }

  void onFlow(const net::UdpFlowEvent& flowEvent) {
    std::cerr << "flow=" << flowEvent.id
              << " backend=" << formatEndpoint(flowEvent.backend)
              << " reason=" << flowEvent.reason;
    if (flowEvent.error) std::cerr << " errno=" << flowEvent.error;
    std::cerr << '\n';
  }

  void onDiagnostic(const std::string& reason, int error) {
    std::cerr << "UDP " << reason;
    if (error) std::cerr << " errno=" << error;
    std::cerr << '\n';
  }

  const Config& config_;

  std::unique_ptr<HealthSelection> selection_;
  MetricsService metrics_;
};
}  // namespace

int runUdpService(const Config& config) {
  if (config.protocol != Protocol::kUdp)
    throw std::invalid_argument("UDP 入口仅支持 UDP 协议");

  UdpServiceRuntime runtime(config);
  return runtime.runUdpServiceLifecycle();
}
}  // namespace l4lb
