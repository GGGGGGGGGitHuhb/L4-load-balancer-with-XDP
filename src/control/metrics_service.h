#pragma once
#include <memory>

#include "control/health_selection.h"
#include "metrics/metrics.h"

namespace l4lb {
/** 产品开关边界：off没有collector/output/周期回调。 */
class MetricsService {
 public:
  MetricsService(const Config& config,
                 const std::unique_ptr<HealthSelection>& selection);

  bool enabled() const { return bool(collector_); }

  void event(net::StatEvent e) noexcept { collector_->update(e); }

  void ready() noexcept {
    if (output_) output_->ready();
  }

  void maintenance() noexcept {
    if (output_) output_->maintenance();
  }

  void finish(bool error) noexcept;

 private:
  bool ended_ = false;
  std::unique_ptr<metrics::Collector> collector_;
  std::unique_ptr<metrics::Output> output_;
};
}  // namespace l4lb
