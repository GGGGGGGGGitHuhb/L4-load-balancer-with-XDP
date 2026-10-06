#pragma once
#include <array>
#include <chrono>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "config/Config.h"
#include "net/Statistics.h"

namespace l4lb::metrics {
enum class BackendHealth { kDisabled, kUnknown, kHealthy, kUnhealthy };

struct BackendHealthSnapshot {
  BackendHealth health = BackendHealth::kDisabled;
  bool eligible = true;
};

/** 固定大小的只读副本；active为真实gauge，累计量饱和。 */
struct MetricsSnapshot {
  Protocol protocol = Protocol::kTcp;
  std::uint64_t seq = 0, uptimeMs = 0;

  std::uint64_t sessionsCreatedTotal = 0, sessionsClosedTotal = 0,
                sessionsActive = 0;

  std::uint64_t bytesC2bTotal = 0, bytesB2cTotal = 0;
  std::uint64_t datagramsC2bTotal = 0, datagramsB2cTotal = 0;

  std::uint64_t rejectedTotal = 0, droppedDatagramsTotal = 0;
  std::uint64_t errorsTotal = 0, timeoutsTotal = 0;
  bool counterSaturated = false;

  std::size_t backendCount = 0;
  std::array<BackendHealthSnapshot, 256> backends{};
};

class MetricsCollector {
  friend struct TestAccess;

 public:
  MetricsCollector(Protocol protocol, std::size_t backendCount);

  /** 无分配/无异常；非法生命周期返回false，绝不钳零掩盖重复关闭。 */
  bool recordStatEvent(net::StatEvent event) noexcept;

  MetricsSnapshot snapshot() const noexcept { return data_; }

 private:
  void incrementCounter(std::uint64_t& value,
                        std::uint64_t incrementAmount) noexcept;

  MetricsSnapshot data_;
};

/** schema=1固定字段顺序；返回一条含前缀和换行的完整行。 */
std::string formatMetricsSnapshot(const MetricsSnapshot& snapshot,
                                  std::string_view phase);

using Clock = std::chrono::steady_clock;

struct MetricsOutputOptions {
  std::function<Clock::time_point()> now;

  std::function<std::ptrdiff_t(std::string_view)> writer;
  std::function<std::string(const MetricsSnapshot&, std::string_view)>
      formatter;
};

/** 同步单次提交。失败禁用，不重试或递归报告错误；慢sink仍可阻塞。 */
class MetricsOutput {
  friend struct TestAccess;

 public:
  using BackendHealthProvider =
      std::function<void(std::span<BackendHealthSnapshot>)>;

  MetricsOutput(MetricsCollector& collector, BackendHealthProvider backends,
                MetricsOutputOptions options = {});

  void emitReadySnapshot() noexcept;
  void emitPeriodicSnapshotIfDue() noexcept;
  void emitFinalSnapshot(bool error) noexcept;

  bool enabled() const noexcept { return enabled_; }

 private:
  Clock::time_point currentTime() const;
  void emitSnapshot(std::string_view phase) noexcept;

  MetricsCollector& collector_;
  BackendHealthProvider backendHealthProvider_;
  MetricsOutputOptions options_;

  Clock::time_point startTime_, nextSnapshotTime_{};

  std::uint64_t seq_ = 0;
  bool readyEmitted_ = false, finalized_ = false, enabled_ = true;
};
}  // namespace l4lb::metrics
