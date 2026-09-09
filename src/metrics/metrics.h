#pragma once
#include <array>
#include <chrono>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "config/config.h"
#include "net/statistics.h"

namespace l4lb::metrics {
enum class Health { Disabled, Unknown, Healthy, Unhealthy };

struct Backend {
  Health health = Health::Disabled;
  bool eligible = true;
};

/** 固定大小的只读副本；active为真实gauge，累计量饱和。 */
struct Snapshot {
  Protocol protocol = Protocol::kTcp;
  std::uint64_t seq = 0, uptime_ms = 0;
  std::uint64_t sessions_created_total = 0, sessions_closed_total = 0,
                sessions_active = 0;
  std::uint64_t bytes_c2b_total = 0, bytes_b2c_total = 0;
  std::uint64_t datagrams_c2b_total = 0, datagrams_b2c_total = 0;
  std::uint64_t rejected_total = 0, dropped_datagrams_total = 0;
  std::uint64_t errors_total = 0, timeouts_total = 0;
  bool counter_saturated = false;
  std::size_t backend_count = 0;
  std::array<Backend, 256> backends{};
};

class Collector {
  friend struct TestAccess;

 public:
  Collector(Protocol protocol, std::size_t backend_count);
  /** 无分配/无异常；非法生命周期返回false，绝不钳零掩盖重复关闭。 */
  bool update(net::StatEvent event) noexcept;

  Snapshot snapshot() const noexcept { return data_; }

 private:
  Snapshot data_;
};

/** schema=1固定字段顺序；返回一条含前缀和换行的完整行。 */
std::string format(const Snapshot& snapshot, std::string_view phase);
using Clock = std::chrono::steady_clock;

struct OutputOptions {
  std::function<Clock::time_point()> now;
  std::function<std::ptrdiff_t(std::string_view)> writer;
  std::function<std::string(const Snapshot&, std::string_view)> formatter;
};

/** 同步单次提交。失败禁用，不重试或递归报告错误；慢sink仍可阻塞。 */
class Output {
  friend struct TestAccess;

 public:
  Output(Collector& collector, std::function<void(std::span<Backend>)> backends,
         OutputOptions options = {});
  void ready() noexcept;
  void maintenance() noexcept;
  void finish(bool error) noexcept;

  bool enabled() const noexcept { return enabled_; }

 private:
  Clock::time_point now() const;
  void emit(std::string_view phase) noexcept;
  Collector& collector_;
  std::function<void(std::span<Backend>)> backends_;
  OutputOptions options_;
  Clock::time_point start_, next_{};
  std::uint64_t seq_ = 0;
  bool ready_ = false, ended_ = false, enabled_ = true;
};
}  // namespace l4lb::metrics
