#pragma once
#include <algorithm>

namespace l4lb::health {
enum class HealthStatus { kUnknown, kHealthy, kUnhealthy };

/** 连续结果状态机；不依赖时钟、socket 或业务流量。 */
struct HealthState {
  HealthStatus status = HealthStatus::kUnknown;
  unsigned successes = 0, failures = 0;

  bool applyProbeResult(bool success) {
    auto before = status;
    if (success) {
      failures = 0;
      successes = std::min(successes + 1, 2u);
      if (successes == 2) status = HealthStatus::kHealthy;
    } else {
      successes = 0;
      failures = std::min(failures + 1, 3u);
      if (failures == 3) status = HealthStatus::kUnhealthy;
    }
    return before != status;
  }
};

inline const char* healthStatusName(HealthStatus healthStatus) {
  switch (healthStatus) {
    case HealthStatus::kUnknown:
      return "Unknown";
    case HealthStatus::kHealthy:
      return "Healthy";
    case HealthStatus::kUnhealthy:
      return "Unhealthy";
  }
  return "invalid";
}
}  // namespace l4lb::health
