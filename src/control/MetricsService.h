#pragma once
#include <memory>

#include "control/HealthSelection.h"
#include "metrics/Metrics.h"

namespace l4lb {
/** 产品开关边界：off没有collector/output/周期回调。 */
class MetricsService {
 public:
  MetricsService(const Config& config,
                 const std::unique_ptr<HealthSelection>& selection);

  bool enabled() const { return bool(collector_); }

  void recordStatEvent(net::StatEvent statEvent) noexcept {
    collector_->recordStatEvent(statEvent);
  }

  void emitReadySnapshot() noexcept {
    if (output_) output_->emitReadySnapshot();
  }

  void emitPeriodicSnapshotIfDue() noexcept {
    if (output_) output_->emitPeriodicSnapshotIfDue();
  }

  void finishMetrics(bool error) noexcept;

 private:
  bool finalized_ = false;

  std::unique_ptr<metrics::MetricsCollector> collector_;
  std::unique_ptr<metrics::MetricsOutput> output_;
};
}  // namespace l4lb
