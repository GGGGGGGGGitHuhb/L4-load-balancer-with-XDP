#pragma once
#include <algorithm>

namespace l4lb::health {
enum class Status { Unknown, Healthy, Unhealthy };

/** 连续结果状态机；不依赖时钟、socket 或业务流量。 */
struct State {
  Status status = Status::Unknown;
  unsigned successes = 0, failures = 0;

  bool complete(bool success) {
    auto before = status;
    if (success) {
      failures = 0;
      successes = std::min(successes + 1, 2u);
      if (successes == 2) status = Status::Healthy;
    } else {
      successes = 0;
      failures = std::min(failures + 1, 3u);
      if (failures == 3) status = Status::Unhealthy;
    }
    return before != status;
  }
};

inline const char* name(Status s) {
  switch (s) {
    case Status::Unknown:
      return "Unknown";
    case Status::Healthy:
      return "Healthy";
    case Status::Unhealthy:
      return "Unhealthy";
  }
  return "invalid";
}
}  // namespace l4lb::health
