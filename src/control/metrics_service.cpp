#include "control/metrics_service.h"

#include <stdexcept>

namespace l4lb {
MetricsService::MetricsService(
    const Config& config, const std::unique_ptr<HealthSelection>& selection) {
  if (config.metrics == MetricsKind::kOff) return;
  if (config.metrics != MetricsKind::kStderr)
    throw std::invalid_argument("invalid metrics");
  collector_ = std::make_unique<metrics::Collector>(config.protocol,
                                                    config.backends.size());
  output_ = std::make_unique<metrics::Output>(
      *collector_, [&config, &selection](std::span<metrics::Backend> backends) {
        if (selection)
          selection->copy_health(backends);
        else
          for (auto& backend : backends)
            backend = config.health_check == HealthCheck::kOff
                          ? metrics::Backend{}
                          : metrics::Backend{metrics::Health::Unknown, false};
      });
}

void MetricsService::finish(bool error) noexcept {
  if (!output_ || ended_) return;
  ended_ = true;
  if (error) collector_->update({net::StatKind::Error});
  output_->finish(error);
}
}  // namespace l4lb
