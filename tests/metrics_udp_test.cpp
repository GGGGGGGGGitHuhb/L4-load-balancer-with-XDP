#include <filesystem>
#include <iostream>

#include "metrics/metrics.h"
#include "net/udp_reactor.cpp"

namespace l4lb::net {
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}

struct UdpTestAccess {
  static void received_failures() {
    // 真实接收nonce，再注入本地事务故障；不宣称自然网络异常。
    for (int mode = 0; mode < 5; ++mode) {
      metrics::Collector counts(Protocol::kUdp, 1);
      UdpCallbacks cb;
      cb.statistics = [&](StatEvent e) {
        check(counts.update(e), "received packet lifecycle");
      };
      cb.select_backend = [&]() -> std::optional<Endpoint> {
        if (mode == 2) throw std::runtime_error("selection-original");
        if (mode == 3) return std::nullopt;
        return Endpoint{{127, 0, 0, 1}, 1234};
      };
      UdpOptions opt;
      if (mode == 1) opt.setup_error = [](const char*, int) { return ENOMEM; };
      if (mode == 4)
        opt.sendmsg_call = [](int, const msghdr* msg, int) {
          return static_cast<ssize_t>(msg->msg_iov[0].iov_len);
        };
      bool threw = false;
      {
        UdpReactor reactor({{127, 0, 0, 1}, 0}, cb, opt);
        sockaddr_in destination{};
        socklen_t length = sizeof(destination);
        check(getsockname(reactor.listener_.get(),
                          reinterpret_cast<sockaddr*>(&destination),
                          &length) == 0,
              "received test listener address");
        Fd sender(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
        check(sendto(sender.get(), "nonce", 5, 0,
                     reinterpret_cast<sockaddr*>(&destination), length) == 5,
              "real nonce send");
        if (mode == 0) reactor.epoll_.reset();
        try {
          reactor.listener_read();
        } catch (const std::system_error& error) {
          check(mode == 0 && error.code().value() == EBADF,
                "original registration exception");
          threw = true;
          counts.update({StatKind::Error});  // control统一计一次的边界。
        } catch (const std::runtime_error& error) {
          check(mode == 2 && std::string(error.what()) == "selection-original",
                "original selection exception");
          threw = true;
          counts.update({StatKind::Error});
        }
        check(threw == (mode == 0 || mode == 2), "expected exception path");
        if (mode == 4)
          check(counts.snapshot().sessions_active == 1,
                "success created before cleanup");
      }
      auto s = counts.snapshot();
      check(s.sessions_active == 0 &&
                s.sessions_created_total == (mode == 4 ? 1u : 0u) &&
                s.sessions_closed_total == s.sessions_created_total,
            "received packet no phantom lifecycle");
      check(s.errors_total == (mode <= 2 ? 1u : 0u),
            "received packet error once at proper boundary");
      check(s.dropped_datagrams_total == (mode == 4 ? 0u : 1u),
            "received packet exactly one drop including exception");
      check(s.rejected_total == (mode == 3 ? 1u : 0u),
            "received packet rejected classification");
      check(s.bytes_c2b_total == (mode == 4 ? 5u : 0u) &&
                s.datagrams_c2b_total == (mode == 4 ? 1u : 0u),
            "received packet success control");
    }
  }

  static void run() {
    metrics::Collector counts(Protocol::kUdp, 1);
    UdpCallbacks cb;
    unsigned logs = 0;
    cb.statistics = [&](StatEvent e) {
      check(counts.update(e), "UDP model lifecycle");
    };
    cb.select_backend = [] { return Endpoint{{127, 0, 0, 1}, 1234}; };
    cb.diagnostic = [&](const std::string&, int) { ++logs; };
    UdpOptions opt;
    auto time = UClock::time_point{};
    opt.now = [&] { return time; };
    opt.sendmsg_call = [](int, const msghdr* msg, int) {
      return static_cast<ssize_t>(msg->msg_iov[0].iov_len);
    };
    FlowKey key{{{127, 0, 0, 1}, 9999}, {127, 0, 0, 1}};
    {
      UdpReactor reactor({{127, 0, 0, 1}, 0}, cb, opt);
      auto token = reactor.create(key);
      check(token != 0 && counts.snapshot().sessions_active == 1,
            "UDP committed creation");
      auto& flow = *reactor.flows_.at(token);
      reactor.send_packet(flow, 0, false);
      reactor.send_packet(flow, 0, true);
      check(counts.snapshot().datagrams_c2b_total == 1 &&
                counts.snapshot().datagrams_b2c_total == 1 &&
                counts.snapshot().bytes_c2b_total == 0,
            "UDP zero datagram must count");
      reactor.send_packet(flow, 9, false);
      reactor.send_packet(flow, 5, true);
      check(counts.snapshot().bytes_c2b_total == 9 &&
                counts.snapshot().bytes_b2c_total == 5,
            "UDP successful bytes");
      opt.sendmsg_call = [](int, const msghdr*, int) {
        errno = EAGAIN;
        return -1;
      };
      reactor.send_packet(flow, 9, false);
      check(counts.snapshot().dropped_datagrams_total == 1 &&
                counts.snapshot().errors_total == 0,
            "EAGAIN drop not error");
      opt.sendmsg_call = [](int, const msghdr*, int) {
        errno = ENOBUFS;
        return -1;
      };
      logs = 0;
      for (int i = 0; i < 3; ++i) reactor.send_packet(flow, 9, false);
      check(counts.snapshot().errors_total == 3 &&
                counts.snapshot().dropped_datagrams_total == 4 && logs <= 1,
            "UDP errors before diagnostic rate limit");
      opt.sendmsg_call = [](int, const msghdr*, int) { return 2; };
      reactor.send_packet(flow, 9, true);
      check(counts.snapshot().errors_total == 4 &&
                counts.snapshot().dropped_datagrams_total == 5 &&
                counts.snapshot().bytes_b2c_total == 5,
            "short send no successful bytes");
      int attempts = 0;
      opt.sendmsg_call = [&](int, const msghdr*, int) {
        ++attempts;
        errno = EINTR;
        return -1;
      };
      reactor.send_packet(flow, 9, false);
      check(attempts == 4 && counts.snapshot().errors_total == 4 &&
                counts.snapshot().dropped_datagrams_total == 6,
            "EINTR exhausted drop only");
      opt.recvmsg_call = [](int, msghdr*, int) {
        errno = ENOBUFS;
        return -1;
      };
      for (int i = 0; i < 3; ++i) reactor.backend_read(token);
      check(counts.snapshot().errors_total == 7 &&
                counts.snapshot().dropped_datagrams_total == 6,
            "recv errors no fictional drop");
      opt.socket_error_call = [](int, int*) {
        errno = EIO;
        return -1;
      };
      reactor.dispatch(token, EPOLLERR);
      check(counts.snapshot().errors_total == 8 &&
                counts.snapshot().sessions_closed_total == 1,
            "SO_ERROR failure once");
      opt.setup_error = [](const char*, int) { return ENOMEM; };
      check(!reactor.create(key) && counts.snapshot().errors_total == 9 &&
                counts.snapshot().sessions_created_total == 1,
            "setup rollback no created");
      opt.setup_error = {};
      token = reactor.create(key);
      time += opt.idle_timeout;
      reactor.expire();
      check(counts.snapshot().timeouts_total == 1 &&
                counts.snapshot().errors_total == 9 &&
                counts.snapshot().sessions_active == 0,
            "UDP idle only timeout");
      token = reactor.create(key);
      reactor.dispatch(token, EPOLLHUP);
      check(counts.snapshot().errors_total == 10, "backend HUP error");
      token = reactor.create(key);
      // 输入已取得的数据报，故障注入仍走listener_read/create的生产落点。
      int supplied = 0;
      opt.recvmsg_call = [&](int, msghdr* msg, int) -> ssize_t {
        if (supplied++) {
          errno = EAGAIN;
          return -1;
        }
        msg->msg_flags = MSG_TRUNC;
        return 9;
      };
      reactor.listener_read();
      check(counts.snapshot().dropped_datagrams_total == 7,
            "truncated acquired packet drop");
      supplied = 0;
      opt.recvmsg_call = [&](int, msghdr* msg, int) -> ssize_t {
        if (supplied++) {
          errno = EAGAIN;
          return -1;
        }
        msg->msg_namelen = 0;
        return 9;
      };
      reactor.listener_read();
      check(counts.snapshot().dropped_datagrams_total == 8,
            "invalid metadata drop");
    }
    check(counts.snapshot().sessions_active == 0 &&
              counts.snapshot().sessions_created_total ==
                  counts.snapshot().sessions_closed_total,
          "UDP stop cleanup");
  }
};
}  // namespace
}  // namespace l4lb::net

int main(int argc, char**) {
  auto count_fds = [] {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator{});
  };
  auto before = count_fds();
  try {
    l4lb::net::UdpTestAccess::run();
    l4lb::net::UdpTestAccess::received_failures();
    // 仅测试进程的负向模式：证明fd断言失败可传播为非零退出。
    if (argc > 1)
      static_cast<void>(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (count_fds() != before)
      throw std::runtime_error("metrics reactor fd leak");
    std::cout
        << "PASS UDP actual event paths with controlled syscall results: "
           "zero/short/EAGAIN/EINTR/pressure/recv/setup/HUP/timeout/cleanup\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
