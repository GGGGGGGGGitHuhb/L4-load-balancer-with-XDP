#include "control/MetricsService.h"

#include <stdexcept>

namespace l4lb {
MetricsService::MetricsService(
    const Config& config, const std::unique_ptr<HealthSelection>& selection) {
  if (config.metrics == MetricsKind::kOff) return;
  if (config.metrics != MetricsKind::kStderr)
    throw std::invalid_argument("invalid metrics");

  collector_ = std::make_unique<metrics::MetricsCollector>(
      config.protocol, config.backends.size());

  output_ = std::make_unique<metrics::MetricsOutput>(
      *collector_, [&config, &selection](
                       std::span<metrics::BackendHealthSnapshot> backends) {
        if (selection)
          selection->copyBackendHealth(backends);
        else
          for (auto& backend : backends)
            backend = config.healthCheck == HealthCheck::kOff
                          ? metrics::BackendHealthSnapshot{}
                          : metrics::BackendHealthSnapshot{
                                metrics::BackendHealth::kUnknown, false};
      });
}

void MetricsService::finishMetrics(bool error) noexcept {
  if (!output_ || finalized_) return;
  finalized_ = true;
  if (error) collector_->recordStatEvent({net::StatKind::kError});
  output_->emitFinalSnapshot(error);
}
}  // namespace l4lb
