#include "control/HealthSelection.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace l4lb {
namespace {
bool isBackendEligible(bool value) { return value; }
}  // namespace

std::optional<std::size_t> selectEligibleBackendIndex(
    BackendScheduler& scheduler, const std::vector<bool>& eligible) {
  if (std::none_of(eligible.begin(), eligible.end(), isBackendEligible))
    return std::nullopt;

  for (std::size_t selectionAttempt = 0; selectionAttempt < eligible.size();
       ++selectionAttempt) {
    auto backendIndex = scheduler.selectNextBackendIndex();
    if (eligible.at(backendIndex)) return backendIndex;
  }
  throw std::logic_error("scheduler did not visit eligible backend");
}

HealthSelection::HealthSelection(const Config& config)
    : config_(config),
      scheduler_(
          createBackendScheduler(config.scheduler, config.backends.size())) {
  if (config.healthCheck == HealthCheck::kOff) return;
  if (config.healthCheck != HealthCheck::kTcpConnect)
    throw std::invalid_argument("invalid health_check");

  checker_ = std::make_unique<health::TcpHealthChecker>(config.backends);
  checker_->setHealthChangeCallback(
      [this](const health::HealthChange& healthChange) {
        onHealthChange(healthChange);
      });
  if (config.protocol == Protocol::kUdp)
    std::cerr << "UDP health_check=tcp_connect：相同 IP/端口的 TCP "
                 "健康端点必须代表 UDP 服务；TCP 握手不是 UDP 协议健康证明。\n";
}

std::optional<Endpoint> HealthSelection::selectBackend() {
  if (!checker_) return config_.backends[scheduler_->selectNextBackendIndex()];

  checker_->pollHealthProbes();  // 长批次内每次新选择仍先处理当前截止和事件。
  std::vector<bool> eligible(checker_->backendCount());
  for (std::size_t backendIndex = 0; backendIndex < eligible.size();
       ++backendIndex)
    eligible[backendIndex] = checker_->backendState(backendIndex).status ==
                             health::HealthStatus::kHealthy;

  auto index = selectEligibleBackendIndex(*scheduler_, eligible);
  if (index) return config_.backends[*index];

  auto time = health::Clock::now();
  if (!lastUnavailableLogTime_ ||
      time - *lastUnavailableLogTime_ >= std::chrono::seconds(1)) {
    std::cerr << "no_healthy_backend\n";
    lastUnavailableLogTime_ = time;
  }
  return std::nullopt;
}

void HealthSelection::copyBackendHealth(
    std::span<metrics::BackendHealthSnapshot> backends) const {
  if (backends.size() != config_.backends.size())
    throw std::invalid_argument("health snapshot size");

  for (std::size_t backendIndex = 0; backendIndex < backends.size();
       ++backendIndex) {
    if (!checker_) {
      backends[backendIndex] = {};
      continue;
    }
    auto state = checker_->backendState(backendIndex).status;
    auto health = state == health::HealthStatus::kUnknown
                      ? metrics::BackendHealth::kUnknown
                      : (state == health::HealthStatus::kHealthy
                             ? metrics::BackendHealth::kHealthy
                             : metrics::BackendHealth::kUnhealthy);
    backends[backendIndex] = {health, state == health::HealthStatus::kHealthy};
  }
}

void HealthSelection::onHealthChange(const health::HealthChange& healthChange) {
  const auto& endpoint = config_.backends[healthChange.backend];
  std::cerr << "health backend=" << unsigned(endpoint.address[0]) << '.'
            << unsigned(endpoint.address[1]) << '.'
            << unsigned(endpoint.address[2]) << '.'
            << unsigned(endpoint.address[3]) << ':' << endpoint.port
            << " from=" << health::healthStatusName(healthChange.from)
            << " to=" << health::healthStatusName(healthChange.to)
            << " reason=" << healthChange.reason;
  if (healthChange.error) std::cerr << " errno=" << healthChange.error;
  std::cerr << '\n';
}
}  // namespace l4lb
