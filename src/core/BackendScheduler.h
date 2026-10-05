#pragma once
#include <cstddef>
#include <memory>

#include "config/Config.h"

namespace l4lb {
/** 固定池的纯索引策略；每次 next 恰好消耗一次选择。 */
class BackendScheduler {
 public:
  virtual ~BackendScheduler() = default;

  virtual std::size_t selectNextBackendIndex() = 0;
};

/** 空池或未知策略抛 invalid_argument，不探测后端。 */
std::unique_ptr<BackendScheduler> createBackendScheduler(
    SchedulerKind kind, std::size_t backendCount);
}  // namespace l4lb
