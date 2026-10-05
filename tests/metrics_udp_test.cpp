#include <filesystem>
#include <iostream>

#include "metrics/Metrics.h"
#include "net/UdpReactor.cpp"

namespace l4lb::net {
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}

struct UdpTestAccess {
  static void received_failures() {
    // 真实接收nonce，再注入本地事务故障；不宣称自然网络异常。
    for (int mode = 0; mode < 5; ++mode) {
      metrics::MetricsCollector counts(Protocol::kUdp, 1);
      UdpReactorCallbacks cb;
      cb.statistics = [&](StatEvent e) {
        check(counts.recordStatEvent(e), "received packet lifecycle");
      };
      cb.select_backend = [&]() -> std::optional<Endpoint> {
        if (mode == 2) throw std::runtime_error("selection-original");
        if (mode == 3) return std::nullopt;
        return Endpoint{{127, 0, 0, 1}, 1234};
      };
      UdpReactorOptions opt;
      if (mode == 1) opt.setupError = [](const char*, int) { return ENOMEM; };
      if (mode == 4)
        opt.sendmsgCall = [](int, const msghdr* msg, int) {
          return static_cast<ssize_t>(msg->msg_iov[0].iov_len);
        };
      bool threw = false;
      {
        UdpReactor reactor({{127, 0, 0, 1}, 0}, cb, opt);
        sockaddr_in destination{};
        socklen_t length = sizeof(destination);
        check(getsockname(reactor.listenerFd_.fd(),
                          reinterpret_cast<sockaddr*>(&destination),
                          &length) == 0,
              "received test listener address");
        Fd sender(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
        check(sendto(sender.fd(), "nonce", 5, 0,
                     reinterpret_cast<sockaddr*>(&destination), length) == 5,
              "real nonce send");
        if (mode == 0) reactor.epollFd_.closeFd();
        try {
          reactor.handleListenerRead();
        } catch (const std::system_error& error) {
          check(mode == 0 && error.code().value() == EBADF,
                "original registration exception");
          threw = true;
          counts.recordStatEvent(
              {StatKind::kError});  // control统一计一次的边界。
        } catch (const std::runtime_error& error) {
          check(mode == 2 && std::string(error.what()) == "selection-original",
                "original selection exception");
          threw = true;
          counts.recordStatEvent({StatKind::kError});
        }
        check(threw == (mode == 0 || mode == 2), "expected exception path");
        if (mode == 4)
          check(counts.snapshot().sessionsActive == 1,
                "success created before cleanup");
      }
      auto s = counts.snapshot();
      check(s.sessionsActive == 0 &&
                s.sessionsCreatedTotal == (mode == 4 ? 1u : 0u) &&
                s.sessionsClosedTotal == s.sessionsCreatedTotal,
            "received packet no phantom lifecycle");
      check(s.errorsTotal == (mode <= 2 ? 1u : 0u),
            "received packet error once at proper boundary");
      check(s.droppedDatagramsTotal == (mode == 4 ? 0u : 1u),
            "received packet exactly one drop including exception");
      check(s.rejectedTotal == (mode == 3 ? 1u : 0u),
            "received packet rejected classification");
      check(s.bytesC2bTotal == (mode == 4 ? 5u : 0u) &&
                s.datagramsC2bTotal == (mode == 4 ? 1u : 0u),
            "received packet success control");
    }
  }

  static void run() {
    metrics::MetricsCollector counts(Protocol::kUdp, 1);
    UdpReactorCallbacks cb;
    unsigned logs = 0;
    cb.statistics = [&](StatEvent e) {
      check(counts.recordStatEvent(e), "UDP model lifecycle");
    };
    cb.select_backend = [] { return Endpoint{{127, 0, 0, 1}, 1234}; };
    cb.diagnostic = [&](const std::string&, int) { ++logs; };
    UdpReactorOptions opt;
    auto time = UClock::time_point{};
    opt.now = [&] { return time; };
    opt.sendmsgCall = [](int, const msghdr* msg, int) {
      return static_cast<ssize_t>(msg->msg_iov[0].iov_len);
    };
    UdpFlowKey key{{{127, 0, 0, 1}, 9999}, {127, 0, 0, 1}};
    {
      UdpReactor reactor({{127, 0, 0, 1}, 0}, cb, opt);
      auto token = reactor.createFlow(key);
      check(token != 0 && counts.snapshot().sessionsActive == 1,
            "UDP committed creation");
      auto& flow = *reactor.flows_.at(token);
      reactor.sendFlowDatagram(flow, 0, false);
      reactor.sendFlowDatagram(flow, 0, true);
      check(counts.snapshot().datagramsC2bTotal == 1 &&
                counts.snapshot().datagramsB2cTotal == 1 &&
                counts.snapshot().bytesC2bTotal == 0,
            "UDP zero datagram must count");
      reactor.sendFlowDatagram(flow, 9, false);
      reactor.sendFlowDatagram(flow, 5, true);
      check(counts.snapshot().bytesC2bTotal == 9 &&
                counts.snapshot().bytesB2cTotal == 5,
            "UDP successful bytes");
      opt.sendmsgCall = [](int, const msghdr*, int) {
        errno = EAGAIN;
        return -1;
      };
      reactor.sendFlowDatagram(flow, 9, false);
      check(counts.snapshot().droppedDatagramsTotal == 1 &&
                counts.snapshot().errorsTotal == 0,
            "EAGAIN drop not error");
      opt.sendmsgCall = [](int, const msghdr*, int) {
        errno = ENOBUFS;
        return -1;
      };
      logs = 0;
      for (int i = 0; i < 3; ++i) reactor.sendFlowDatagram(flow, 9, false);
      check(counts.snapshot().errorsTotal == 3 &&
                counts.snapshot().droppedDatagramsTotal == 4 && logs <= 1,
            "UDP errors before diagnostic rate limit");
      opt.sendmsgCall = [](int, const msghdr*, int) { return 2; };
      reactor.sendFlowDatagram(flow, 9, true);
      check(counts.snapshot().errorsTotal == 4 &&
                counts.snapshot().droppedDatagramsTotal == 5 &&
                counts.snapshot().bytesB2cTotal == 5,
            "short send no successful bytes");
      int attempts = 0;
      opt.sendmsgCall = [&](int, const msghdr*, int) {
        ++attempts;
        errno = EINTR;
        return -1;
      };
      reactor.sendFlowDatagram(flow, 9, false);
      check(attempts == 4 && counts.snapshot().errorsTotal == 4 &&
                counts.snapshot().droppedDatagramsTotal == 6,
            "EINTR exhausted drop only");
      opt.recvmsgCall = [](int, msghdr*, int) {
        errno = ENOBUFS;
        return -1;
      };
      for (int i = 0; i < 3; ++i) reactor.handleBackendRead(token);
      check(counts.snapshot().errorsTotal == 7 &&
                counts.snapshot().droppedDatagramsTotal == 6,
            "recv errors no fictional drop");
      opt.socketErrorCall = [](int, int*) {
        errno = EIO;
        return -1;
      };
      reactor.dispatchFlowEvent(token, EPOLLERR);
      check(counts.snapshot().errorsTotal == 8 &&
                counts.snapshot().sessionsClosedTotal == 1,
            "SO_ERROR failure once");
      opt.setupError = [](const char*, int) { return ENOMEM; };
      check(!reactor.createFlow(key) && counts.snapshot().errorsTotal == 9 &&
                counts.snapshot().sessionsCreatedTotal == 1,
            "setup rollback no created");
      opt.setupError = {};
      token = reactor.createFlow(key);
      time += opt.idleTimeout;
      reactor.expireIdleFlows();
      check(counts.snapshot().timeoutsTotal == 1 &&
                counts.snapshot().errorsTotal == 9 &&
                counts.snapshot().sessionsActive == 0,
            "UDP idle only timeout");
      token = reactor.createFlow(key);
      reactor.dispatchFlowEvent(token, EPOLLHUP);
      check(counts.snapshot().errorsTotal == 10, "backend HUP error");
      token = reactor.createFlow(key);
      // 输入已取得的数据报，故障注入仍走listener_read/create的生产落点。
      int supplied = 0;
      opt.recvmsgCall = [&](int, msghdr* msg, int) -> ssize_t {
        if (supplied++) {
          errno = EAGAIN;
          return -1;
        }
        msg->msg_flags = MSG_TRUNC;
        return 9;
      };
      reactor.handleListenerRead();
      check(counts.snapshot().droppedDatagramsTotal == 7,
            "truncated acquired packet drop");
      supplied = 0;
      opt.recvmsgCall = [&](int, msghdr* msg, int) -> ssize_t {
        if (supplied++) {
          errno = EAGAIN;
          return -1;
        }
        msg->msg_namelen = 0;
        return 9;
      };
      reactor.handleListenerRead();
      check(counts.snapshot().droppedDatagramsTotal == 8,
            "invalid metadata drop");
    }
    check(counts.snapshot().sessionsActive == 0 &&
              counts.snapshot().sessionsCreatedTotal ==
                  counts.snapshot().sessionsClosedTotal,
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
