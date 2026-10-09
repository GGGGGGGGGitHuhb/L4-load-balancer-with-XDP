#include "control/TcpService.h"

#include <iostream>
#include <stdexcept>
#include <system_error>

#include "control/MetricsService.h"
#include "net/TcpReactor.h"

namespace l4lb {
namespace {
std::string formatEndpoint(const Endpoint& endpoint) {
  return std::to_string(endpoint.address[0]) + "." + std::to_string(endpoint.address[1]) + "." +
         std::to_string(endpoint.address[2]) + "." + std::to_string(endpoint.address[3]) + ":" +
         std::to_string(endpoint.port);
}

class TcpServiceRuntime {
 public:
  explicit TcpServiceRuntime(const Config& config)
      : config_(config), metrics_(config, selection_) {}

  int runTcpServiceLifecycle();

 private:
  void bindReactorCallbacks(net::TcpReactorCallbacks& callbacks);

  std::optional<Endpoint> selectBackend() { return selection_->selectBackend(); }

  void onStatistics(net::StatEvent statEvent) noexcept { metrics_.recordStatEvent(statEvent); }

  void onMaintenance();
  void onReady();

  void onStopping(const net::StopEvent& stopEvent);
  void onSession(const net::SessionEvent& sessionEvent);
  void onDiagnostic(const std::string& operation, int error);

  const Config& config_;

  std::unique_ptr<HealthSelection> selection_;

  MetricsService metrics_;
};

int TcpServiceRuntime::runTcpServiceLifecycle() {
  try {
    selection_ = std::make_unique<HealthSelection>(config_);

    net::TcpReactorCallbacks callbacks;
    bindReactorCallbacks(callbacks);

    auto result = net::runTcpReactor(config_.listen, callbacks);

    std::cerr << "TCP "
                 "服务已停止：已尝试有界排空用户态待发队列，不保证在途数据送达\n";
    metrics_.finishMetrics(false);
    return result;
  } catch (...) {
    metrics_.finishMetrics(true);
    throw;
  }
}

void TcpServiceRuntime::bindReactorCallbacks(net::TcpReactorCallbacks& callbacks) {
  callbacks.setBackendSelector([this]() { return selectBackend(); });

  if (metrics_.enabled())
    callbacks.setStatisticsCallback(
        [this](net::StatEvent statEvent) noexcept { onStatistics(statEvent); });
  if (selection_->enabled() || metrics_.enabled())
    callbacks.setMaintenanceCallback([this]() { onMaintenance(); });

  callbacks.setReadyCallback([this]() { onReady(); });
  callbacks.setStoppingCallback([this](const net::StopEvent& stopEvent) { onStopping(stopEvent); });
  callbacks.setSessionCallback(
      [this](const net::SessionEvent& sessionEvent) { onSession(sessionEvent); });
  callbacks.setDiagnosticCallback(
      [this](const std::string& operation, int error) { onDiagnostic(operation, error); });
}

void TcpServiceRuntime::onMaintenance() {
  if (selection_->enabled()) selection_->pollHealthProbes();

  metrics_.emitPeriodicSnapshotIfDue();
}

void TcpServiceRuntime::onReady() {
  std::cout << "TCP 服务已启动：" << formatEndpoint(config_.listen) << std::endl;

  metrics_.emitReadySnapshot();
}

void TcpServiceRuntime::onStopping(const net::StopEvent& stopEvent) {
  std::cerr << "TCP draining pending_c2b=" << stopEvent.pending[0]
            << " pending_b2c=" << stopEvent.pending[1] << " deadline_ns=" << stopEvent.deadlineNs
            << '\n';
}

void TcpServiceRuntime::onSession(const net::SessionEvent& sessionEvent) {
  std::cerr << "session=" << sessionEvent.id << " backend=" << formatEndpoint(sessionEvent.backend)
            << " reason=" << sessionEvent.reason;
  if (!sessionEvent.accepted)
    std::cerr << " sent_c2b=" << sessionEvent.sent[0] << " sent_b2c=" << sessionEvent.sent[1];
  if (sessionEvent.error)
    std::cerr << " errno=" << sessionEvent.error << " "
              << std::generic_category().message(sessionEvent.error);
  std::cerr << '\n';
}

void TcpServiceRuntime::onDiagnostic(const std::string& operation, int error) {
  std::cerr << operation << " errno=" << error << " " << std::generic_category().message(error)
            << '\n';
}

}  // namespace

int runTcpService(const Config& config) {
  if (config.protocol != Protocol::kTcp) throw std::invalid_argument("TCP 入口仅支持 TCP 协议");

  TcpServiceRuntime runtime(config);
  return runtime.runTcpServiceLifecycle();
}
}  // namespace l4lb
