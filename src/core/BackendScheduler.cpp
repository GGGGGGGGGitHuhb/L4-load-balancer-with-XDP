#include "core/BackendScheduler.h"

#include <stdexcept>

#include "core/RoundRobinScheduler.h"

namespace l4lb {
std::unique_ptr<BackendScheduler> createBackendScheduler(
    SchedulerKind kind, std::size_t backendCount) {
  switch (kind) {
    case SchedulerKind::kRoundRobin:
      return std::make_unique<RoundRobinScheduler>(backendCount);
  }
  throw std::invalid_argument("unknown scheduler");
}
}  // namespace l4lb
