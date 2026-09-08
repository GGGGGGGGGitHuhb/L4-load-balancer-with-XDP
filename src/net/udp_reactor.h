#pragma once
#include <sys/socket.h>

#include <chrono>
#include <functional>
#include <string>

#include "config/config.h"
#include "core/udp_flow.h"
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
struct UdpCallbacks {
  std::function<Endpoint()> select_backend;
  std::function<void()> ready;
  std::function<void(const UdpFlowEvent&)> flow;
  std::function<void(const std::string&, int)> diagnostic;
};
/** 内部测试选项；生产不暴露配置项。空注入函数均使用真实 syscall。 */
struct UdpOptions {
  std::size_t max_flows = 1024;
  std::chrono::milliseconds idle_timeout{60000}, poll_interval{100};
  std::size_t receive_size = kUdpPayloadLimit;
  std::function<void(const UdpObservation&)> observe;
  // 返回 0 继续真实调用，非零值模拟该操作 errno。仅 flow 建立事务使用。
  std::function<int(const char*, int)> setup_error;
  std::function<ssize_t(int, const msghdr*, int)> sendmsg_call;
  std::function<ssize_t(int, msghdr*, int)> recvmsg_call;
  std::function<int(int, int*)> socket_error_call;
  std::function<UdpDeadline::Clock::time_point()> now;
};
/** 同步单线程 LT UDP reactor；信号退出 0，不可恢复错误抛异常。 */
int run_udp(const Endpoint& listen, const UdpCallbacks& callbacks,
            const UdpOptions& options = {});
}  // namespace l4lb::net
