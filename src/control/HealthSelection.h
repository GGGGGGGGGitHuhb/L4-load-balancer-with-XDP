#pragma once
#include <optional>

#include "core/BackendScheduler.h"
#include "health/TcpHealthChecker.h"
#include "metrics/Metrics.h"

namespace l4lb {
/** 先检查集合再消耗轮询；空集合不改变 cursor。 */
std::optional<std::size_t> selectEligibleBackendIndex(
    BackendScheduler& scheduler, const std::vector<bool>& eligible);

/** control 独占健康维护和资格过滤，数据面不知道健康策略。 */
class HealthSelection {
 public:
  explicit HealthSelection(const Config& config);

  bool enabled() const { return bool(checker_); }

  void pollHealthProbes() { checker_->pollHealthProbes(); }

  std::optional<Endpoint> selectBackend();

  /** 只复制状态，不tick/调度/创建probe。 */
  void copyBackendHealth(
      std::span<metrics::BackendHealthSnapshot> backends) const;

 private:
  void onHealthChange(const health::HealthChange& healthChange);

  const Config& config_;

  std::unique_ptr<BackendScheduler> scheduler_;

  std::unique_ptr<health::TcpHealthChecker> checker_;

  std::optional<health::Clock::time_point> lastUnavailableLogTime_;
};
}  // namespace l4lb
