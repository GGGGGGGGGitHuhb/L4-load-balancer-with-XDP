// 定向状态测试编译同一实现；friend 仅允许本翻译单元构造稀有事件，
// 不替换生产 I/O 算法，不导出额外 CLI 或运行时接口。
#include <fcntl.h>

#include <iostream>
#include <thread>

#include "../src/net/reactor.cpp"

namespace l4lb::net {
namespace {
void require(bool condition, const char* why) {
  if (!condition) throw std::runtime_error(why);
}

struct ReactorTestAccess {
  static void run_test() {
    Endpoint listen{{127, 0, 0, 1}, 0};
    std::vector<Observation> observations;
    std::vector<SessionEvent> closed;
    Options options;
    bool blocked = true;
    int blocked_fd = -1;
    std::size_t send_calls = 0;
    options.send_call = [&](int fd, const void* data, std::size_t n,
                            int flags) -> ssize_t {
      ++send_calls;
      if (blocked && fd == blocked_fd) {
        errno = EAGAIN;
        return -1;
      }
      return send(fd, data, n, flags);
    };
    options.observe = [&](const Observation& o) { observations.push_back(o); };
    Callbacks callbacks;
    callbacks.select_backend = [] { return Endpoint{{127, 0, 0, 1}, 1}; };
    callbacks.ready = [] {};
    callbacks.session = [&](const SessionEvent& e) { closed.push_back(e); };
    callbacks.diagnostic = [](const std::string&, int) {};
    Reactor reactor(listen, callbacks, options);
    int pair0[2], pair1[2];
    require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                       pair0) == 0,
            "pair0");
    require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                       pair1) == 0,
            "pair1");
    Fd peer0(pair0[1]), peer1(pair1[1]);
    auto session = std::make_unique<Session>();
    session->id = 1;
    session->connecting = false;
    session->ends[0].fd = Fd(pair0[0]);
    session->ends[1].fd = Fd(pair1[0]);
    blocked_fd = pair1[0];
    for (int i = 0; i < 2; ++i)
      session->ends[i].token = reactor.tokens_.add(1, i);
    auto& s = *session;
    reactor.sessions_.emplace(1, std::move(session));
    std::memset(s.pending[0].writable(), 'x', kBufferLimit);
    s.pending[0].append(kBufferLimit);
    s.ends[0].paused = true;
    reactor.update(s);
    const auto old_token = s.ends[0].token;
    reactor.dispatch(old_token, EPOLLHUP);  // 定向注入真实算法的不可屏蔽 HUP。
    require(!s.ends[0].registered && s.ends[0].deferred > Clock::now(),
            "full HUP defers source");
    require(s.pending[0].size() == kBufferLimit, "HUP preserves full queue");
    auto calls = send_calls;
    for (int i = 0; i < 100; ++i) reactor.deadlines();
    require(send_calls == calls, "no retry before 100ms (no busy-loop)");
    require(send(peer1.get(), "reverse", 7, MSG_NOSIGNAL) == 7,
            "reverse peer write");
    std::this_thread::sleep_for(std::chrono::milliseconds(105));
    reactor.deadlines();
    char bytes[65536];
    require(recv(peer0.get(), bytes, sizeof(bytes), 0) == 7 &&
                std::string(bytes, 7) == "reverse",
            "deferred source still supports reverse writes");
    blocked = false;
    reactor.dispatch(s.ends[1].token, EPOLLOUT);
    require(s.pending[0].size() == 0 && !s.ends[0].paused,
            "drain and resume after full HUP");
    require((s.ends[1].interest & EPOLLOUT) == 0,
            "empty output removes EPOLLOUT");
    require(recv(peer1.get(), bytes, sizeof(bytes), 0) == 65536,
            "full original payload delivered");
    for (char byte : bytes) require(byte == 'x', "payload unchanged");
    bool deferred = false, retried = false;
    for (auto& o : observations) {
      deferred |= o.kind == "hup-defer";
      retried |= o.kind == "hup-retry";
    }
    require(deferred && retried, "HUP branch observations");
    const int old_fd = s.ends[0].fd.get();
    reactor.close(1, "test-close", 0);
    reactor.close(1, "duplicate", 0);
    require(reactor.sessions_.empty() && reactor.tokens_.size() == 0 &&
                closed.size() == 1,
            "idempotent close");
    int next_pair[2];
    require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                       next_pair) == 0,
            "next pair");
    Fd next_end(next_pair[0]), next_peer(next_pair[1]);
    if (next_end.get() != old_fd) {
      require(dup2(next_end.get(), old_fd) == old_fd, "forced fd reuse");
      next_end = Fd(old_fd);
    }
    auto next = std::make_unique<Session>();
    next->id = 2;
    next->ends[0].fd = std::move(next_end);
    next->ends[0].token = reactor.tokens_.add(2, 0);
    reactor.sessions_.emplace(2, std::move(next));
    reactor.dispatch(old_token, EPOLLERR | EPOLLHUP | EPOLLIN | EPOLLOUT);
    require(reactor.sessions_.size() == 1 && fcntl(old_fd, F_GETFD) >= 0,
            "stale batch ignores reused fd");
    require(observations.back().kind == "stale-token",
            "stale dispatch observed");
    reactor.close(2, "test-close", 0);
    // Connecting 时前端先关闭写端：不可读应用数据，成功后在同次
    // pump 消费已排队尾字节和 EOF，再传 FIN；后端仍可返回响应。
    int early0[2], early1[2];
    require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                       early0) == 0,
            "early pair0");
    require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                       early1) == 0,
            "early pair1");
    Fd early_front(early0[1]), early_back(early1[1]);
    auto early = std::make_unique<Session>();
    early->id = 3;
    early->ends[0].fd = Fd(early0[0]);
    early->ends[1].fd = Fd(early1[0]);
    for (int side = 0; side < 2; ++side)
      early->ends[side].token = reactor.tokens_.add(3, side);
    auto front_token = early->ends[0].token, back_token = early->ends[1].token;
    reactor.sessions_.emplace(3, std::move(early));
    require(send(early_front.get(), "tail", 4, MSG_NOSIGNAL) == 4,
            "queued tail");
    require(shutdown(early_front.get(), SHUT_WR) == 0, "queued FIN");
    reactor.dispatch(front_token, EPOLLIN | EPOLLRDHUP);
    require(reactor.sessions_.at(3)->pending[0].size() == 0 &&
                !reactor.sessions_.at(3)->ends[0].eof,
            "Connecting does not read front");
    reactor.dispatch(back_token, EPOLLOUT);
    require(recv(early_back.get(), bytes, sizeof(bytes), 0) == 4 &&
                std::string(bytes, 4) == "tail",
            "queued data before FIN");
    require(recv(early_back.get(), bytes, sizeof(bytes), 0) == 0,
            "forward FIN after tail drained");
    require(send(early_back.get(), "after-fin", 9, MSG_NOSIGNAL) == 9,
            "late response");
    require(shutdown(early_back.get(), SHUT_WR) == 0, "response FIN");
    reactor.dispatch(back_token, EPOLLIN | EPOLLRDHUP);
    require(recv(early_front.get(), bytes, sizeof(bytes), 0) == 9 &&
                std::string(bytes, 9) == "after-fin",
            "response preserved after front FIN");
    require(reactor.sessions_.empty() && closed.back().reason == "drained",
            "both drained");
  }
};
}  // namespace
}  // namespace l4lb::net

int main() {
  try {
    sigset_t before{}, after{};
    sigprocmask(SIG_SETMASK, nullptr, &before);
    l4lb::net::ReactorTestAccess::run_test();
    sigprocmask(SIG_SETMASK, nullptr, &after);
    for (int signal : {SIGINT, SIGTERM})
      if (sigismember(&before, signal) != sigismember(&after, signal))
        throw std::runtime_error("signal mask restore");
    std::cout << "reactor state PASS: full queue HUP defer/retry, bounded "
                 "retry, reverse progress, empty-write unsubscribe, same-batch "
                 "stale event with actual fd reuse, idempotent close\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
