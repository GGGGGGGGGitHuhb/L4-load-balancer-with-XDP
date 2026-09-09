#include "core/scheduler.h"

#include <stdexcept>

#include "core/round_robin.h"

namespace l4lb {
std::unique_ptr<Scheduler> make_scheduler(SchedulerKind kind,
                                          std::size_t size) {
  switch (kind) {
    case SchedulerKind::kRoundRobin:
      return std::make_unique<RoundRobin>(size);
  }
  throw std::invalid_argument("unknown scheduler");
}
}  // namespace l4lb
