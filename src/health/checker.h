#pragma once
#include <sys/epoll.h>
#include <sys/socket.h>

#include <chrono>
#include <functional>
#include <vector>

#include "config/config.h"
#include "health/state.h"
#include "net/fd.h"
namespace l4lb::health {
using Clock = std::chrono::steady_clock;
struct Change {
  std::size_t backend;
  Status from, to;
  const char* reason;
  int error;
};
/** 内部注入仅用于确定性验证；生产固定 1s/1s。 */
struct Options {
  std::chrono::milliseconds interval{1000}, timeout{1000};
  std::function<Clock::time_point()> now;
  std::function<int()> socket_call;
  std::function<int(int, const sockaddr*, socklen_t)> connect_call;
  std::function<int(int, int*)> error_call;
  std::function<int(int, int, int, epoll_event*)> ctl_call;
  std::function<int(int, epoll_event*, int)> poll_call;
};
/** 单线程非阻塞探测；独占所有 probe，析构取消不计失败。 */
class Checker {
 public:
  Checker(const std::vector<Endpoint>& endpoints,
          std::function<void(const Change&)> changed = {},
          Options options = {});
  void tick();
  const State& state(std::size_t i) const { return probes_.at(i).state; }
  std::size_t size() const { return probes_.size(); }
  std::size_t active() const;

 private:
  struct Probe {
    State state;
    net::Fd fd;
    std::uint64_t token = 0;
    Clock::time_point next{}, deadline{};
  };
  Clock::time_point now() const;
  void finish(std::size_t i, bool success, const char* reason, int error,
              Clock::time_point time);
  void start(std::size_t i, Clock::time_point time);
  std::vector<Endpoint> endpoints_;
  std::function<void(const Change&)> changed_;
  Options options_;
  net::Fd epoll_;
  std::vector<Probe> probes_;
  std::uint64_t next_token_ = 1;
};
}  // namespace l4lb::health
