#pragma once
#include <optional>

#include "core/scheduler.h"
#include "health/checker.h"
namespace l4lb {
/** 先检查集合再消耗轮询；空集合不改变 cursor。 */
std::optional<std::size_t> eligible_next(Scheduler& scheduler,
                                         const std::vector<bool>& eligible);
/** control 独占健康维护和资格过滤，数据面不知道健康策略。 */
class HealthSelection {
 public:
  explicit HealthSelection(const Config& config);
  bool enabled() const { return bool(checker_); }
  void tick() { checker_->tick(); }
  std::optional<Endpoint> next();

 private:
  const Config& config_;
  std::unique_ptr<Scheduler> scheduler_;
  std::unique_ptr<health::Checker> checker_;
  std::optional<health::Clock::time_point> unavailable_log_;
};
}  // namespace l4lb
