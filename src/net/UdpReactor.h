#pragma once
#include <sys/socket.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>

#include "config/Config.h"
#include "core/UdpFlow.h"
#include "net/Statistics.h"

namespace l4lb::net {
inline constexpr std::size_t kUdpPayloadLimit = 65507;

struct UdpFlowEvent {
  std::uint64_t id;
  Endpoint backend;
  std::string reason;
  int error = 0;
};

struct UdpObservation {
  std::string kind;
  std::uint64_t token;
  int fd;
  std::size_t flows, tokens;
};

struct UdpReactorCallbacks {
  std::function<std::optional<Endpoint>()> select_backend;
  std::function<void()> maintenance;
  std::function<void(StatEvent)> statistics;
  std::function<void()> ready;
  std::function<void(const UdpFlowEvent&)> flow;
  std::function<void(const std::string&, int)> diagnostic;
};

/** 内部测试选项；生产不暴露配置项。空注入函数均使用真实 syscall。 */
struct UdpReactorOptions {
  std::size_t maxFlows = 1024;
  std::chrono::milliseconds idleTimeout{60000}, pollInterval{100};
  std::size_t receiveSize = kUdpPayloadLimit;

  std::function<void(const UdpObservation&)> observe;

  // 返回 0 继续真实调用，非零值模拟该操作 errno。仅 flow 建立事务使用。
  std::function<int(const char*, int)> setupError;
  std::function<ssize_t(int, const msghdr*, int)> sendmsgCall;
  std::function<ssize_t(int, msghdr*, int)> recvmsgCall;
  std::function<int(int, int*)> socketErrorCall;

  std::function<UdpIdleDeadline::Clock::time_point()> now;
};

/** 同步单线程 LT UDP reactor；信号退出 0，不可恢复错误抛异常。 */
int runUdpReactor(const Endpoint& listen, const UdpReactorCallbacks& callbacks,
                  const UdpReactorOptions& options = {});
}  // namespace l4lb::net
