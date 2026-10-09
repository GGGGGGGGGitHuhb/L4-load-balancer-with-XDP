#pragma once
#include <sys/epoll.h>
#include <sys/socket.h>

#include <chrono>
#include <functional>
#include <utility>
#include <vector>

#include "config/Config.h"
#include "health/HealthState.h"
#include "net/Fd.h"

namespace l4lb::health {
using Clock = std::chrono::steady_clock;

struct HealthChange {
  std::size_t backend;
  HealthStatus from, to;
  const char* reason;
  int error;
};

/** 内部注入仅用于确定性验证；生产固定 1s/1s。 */
struct TcpHealthCheckOptions {
  std::chrono::milliseconds interval{1000}, timeout{1000};

  std::function<Clock::time_point()> now;
  std::function<int()> socketCall;
  std::function<int(int, const sockaddr*, socklen_t)> connectCall;
  std::function<int(int, int*)> socketErrorCall;
  std::function<int(int, int, int, epoll_event*)> epollControlCall;
  std::function<int(int, epoll_event*, int)> epollPollCall;
};

/** 单线程非阻塞探测；独占所有 probe，析构取消不计失败。 */
class TcpHealthChecker {
 public:
  using HealthChangeCallback = std::function<void(const HealthChange&)>;

  TcpHealthChecker(const std::vector<Endpoint>& endpoints, TcpHealthCheckOptions options = {});

  void setHealthChangeCallback(HealthChangeCallback healthChangeCallback) {
    healthChangeCallback_ = std::move(healthChangeCallback);
  }

  void pollHealthProbes();

  const HealthState& backendState(std::size_t backendIndex) const {
    return probes_.at(backendIndex).state;
  }

  std::size_t backendCount() const { return probes_.size(); }

  std::size_t activeProbeCount() const;

 private:
  struct Probe {
    HealthState state;
    net::Fd fd;
    std::uint64_t token = 0;

    Clock::time_point nextProbeTime{}, probeDeadline{};
  };

  Clock::time_point currentTime() const;

  void completeBackendProbe(std::size_t backendIndex,
                            bool success,
                            const char* reason,
                            int error,
                            Clock::time_point time);
  void startBackendProbe(std::size_t backendIndex, Clock::time_point time);

  std::vector<Endpoint> endpoints_;
  HealthChangeCallback healthChangeCallback_;

  TcpHealthCheckOptions options_;

  net::Fd epollFd_;
  std::vector<Probe> probes_;

  std::uint64_t nextProbeToken_ = 1;
};
}  // namespace l4lb::health
