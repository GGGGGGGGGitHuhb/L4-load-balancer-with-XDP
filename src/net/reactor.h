#pragma once
#include <sys/socket.h>

#include <chrono>
#include <functional>
#include <string>

#include "config/config.h"
#include "net/state.h"
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
struct Options {
  std::size_t max_sessions = 1024;
  std::chrono::milliseconds connect_timeout{5000}, idle_timeout{60000};
  std::function<void(const Observation&)> observe;
  // 故障注入仅用于内部测试；缺省始终调用真实系统接口。
  std::function<int(int, const sockaddr*, socklen_t)> connect_call;
  std::function<int(int, int*)> socket_error_call;
  std::function<ssize_t(int, const void*, std::size_t, int)> send_call;
  std::function<int(int, sockaddr*, socklen_t*, int)> accept_call;
};
struct Callbacks {
  std::function<Endpoint()> select_backend;
  std::function<void()> ready;
  std::function<void(const SessionEvent&)> session;
  std::function<void(const std::string&, int)> diagnostic;
};
/** 单线程 LT reactor，信号退出返回 0；不可恢复错误抛 system_error。 */
int run(const Endpoint& listen, const Callbacks& callbacks,
        const Options& options = {});
}  // namespace l4lb::net
