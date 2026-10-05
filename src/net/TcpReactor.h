#pragma once
#include <sys/socket.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>

#include "config/Config.h"
#include "net/Statistics.h"
#include "net/TcpState.h"

namespace l4lb::net {
/** 结构化生命周期输出；net 不打印日志或载荷。 */
struct SessionEvent {
  std::uint64_t id;
  Endpoint backend;
  bool accepted;
  std::string reason;
  int error = 0;
  std::uint64_t sent[2]{};
};

/** 内部观测，不是产品指标接口。未设置时没有逐 I/O 输出。 */
struct Observation {
  std::string kind;
  std::uint64_t session = 0;
  int side = -1;
  std::uint64_t value = 0;
  std::size_t sessions = 0, tokens = 0;
};

struct TcpReactorOptions {
  std::size_t maxSessions = 1024;
  std::chrono::milliseconds connectTimeout{5000}, idleTimeout{60000};
  std::chrono::milliseconds drainTimeout{1000};

  std::function<void(const Observation&)> observe;

  // 故障注入仅用于内部测试；缺省始终调用真实系统接口。
  std::function<int(int, const sockaddr*, socklen_t)> connectCall;
  std::function<int(int, int*)> socketErrorCall;
  std::function<ssize_t(int, const void*, std::size_t, int)> sendCall;
  std::function<int(int, sockaddr*, socklen_t*, int)> acceptCall;
};

/** 停止屏障只报告已在用户态的队列，不代表内核中在途数据。 */
struct StopEvent {
  std::uint64_t pending[2]{};
  std::int64_t deadlineNs = 0;
};

struct TcpReactorCallbacks {
  std::function<std::optional<Endpoint>()> select_backend;
  std::function<void()> maintenance;
  std::function<void(StatEvent)> statistics;
  std::function<void()> ready;
  std::function<void(const StopEvent&)> stopping;
  std::function<void(const SessionEvent&)> session;
  std::function<void(const std::string&, int)> diagnostic;
};

/** 单线程 LT reactor，信号退出返回 0；不可恢复错误抛 system_error。 */
int runTcpReactor(const Endpoint& listen, const TcpReactorCallbacks& callbacks,
                  const TcpReactorOptions& options = {});
}  // namespace l4lb::net
