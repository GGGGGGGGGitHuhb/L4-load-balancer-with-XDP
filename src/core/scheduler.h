#pragma once
#include <cstddef>
#include <memory>

#include "config/config.h"
namespace l4lb {
/** 固定池的纯索引策略；每次 next 恰好消耗一次选择。 */
class Scheduler {
 public:
  virtual ~Scheduler() = default;
  virtual std::size_t next() = 0;
};
/** 空池或未知策略抛 invalid_argument，不探测后端。 */
std::unique_ptr<Scheduler> make_scheduler(SchedulerKind kind, std::size_t size);
}  // namespace l4lb
