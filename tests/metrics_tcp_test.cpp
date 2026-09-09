#include <filesystem>
#include <iostream>

#include "metrics/metrics.h"
#include "net/reactor.cpp"

namespace l4lb::net {
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}

Endpoint local(int fd) {
  sockaddr_in a{};
  socklen_t length = sizeof(a);
  check(getsockname(fd, reinterpret_cast<sockaddr*>(&a), &length) == 0,
        "getsockname");
  Endpoint e{{127, 0, 0, 1}, ntohs(a.sin_port)};
  return e;
}

struct ReactorTestAccess {
  static void run() {
    metrics::Collector counts(Protocol::kTcp, 1);
    Callbacks cb;
    cb.statistics = [&](StatEvent e) {
      check(counts.update(e), "TCP model lifecycle");
    };
    cb.ready = [] {};
    cb.session = [](const SessionEvent&) {};
    cb.diagnostic = [](const std::string&, int) {};
    Fd backend(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    auto a = address({{127, 0, 0, 1}, 0});
    check(
        bind(backend.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 &&
            listen(backend.get(), 8) == 0,
        "backend");
    cb.select_backend = [&] { return local(backend.get()); };
    Options opt;
    opt.send_call = [](int fd, const void* data, std::size_t n, int flags) {
      return send(fd, data, std::min(n, std::size_t(7)), flags);
    };
    {
      Reactor reactor({{127, 0, 0, 1}, 0}, cb, opt);
      Fd front(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
      a = address(local(reactor.listener_.get()));
      check(
          connect(front.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0,
          "client connect");
      reactor.accept_sessions();
      check(counts.snapshot().sessions_created_total == 1 &&
                counts.snapshot().sessions_active == 1,
            "real created point");
      auto& s = *reactor.sessions_.begin()->second;
      s.connecting = false;
      std::memcpy(s.pending[0].writable(), "abcdefghijk", 11);
      s.pending[0].append(11);
      std::size_t budget = 7;
      reactor.write_direction(s, 0, budget);
      check(counts.snapshot().bytes_c2b_total == 7 &&
                counts.snapshot().sessions_active == 1,
            "TCP bytes must be immediate before close");
      budget = 4;
      reactor.write_direction(s, 0, budget);
      check(counts.snapshot().bytes_c2b_total == 11, "TCP partial send totals");
      auto id = s.id;
      reactor.close(id, "drained", 0);
      reactor.close(id, "drained", 0);
      check(counts.snapshot().bytes_c2b_total == 11 &&
                counts.snapshot().sessions_closed_total == 1 &&
                counts.snapshot().sessions_active == 0,
            "close no byte duplicate");
      int attempts = 0;
      opt.accept_call = [&](int, sockaddr*, socklen_t*, int) {
        errno = attempts++ < 3 ? ECONNABORTED : EAGAIN;
        return -1;
      };
      reactor.accept_sessions();
      check(counts.snapshot().errors_total == 3,
            "each ignored accept failure counts");
      attempts = 0;
      unsigned logged = 0;
      cb.diagnostic = [&](const std::string&, int) { ++logged; };
      opt.accept_call = [&](int, sockaddr*, socklen_t*, int) {
        errno = EMFILE;
        ++attempts;
        return -1;
      };
      reactor.accept_sessions();
      reactor.listener_retry_ = Clock::time_point{};
      reactor.deadlines();
      reactor.accept_sessions();
      check(counts.snapshot().errors_total == 5 && logged == 1,
            "accept resource errors before suppression");
      opt.accept_call = {};
      reactor.listener_retry_ = Clock::time_point{};
      reactor.deadlines();
      opt.max_sessions = 0;
      Fd rejected(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
      check(connect(rejected.get(), reinterpret_cast<sockaddr*>(&a),
                    sizeof(a)) == 0,
            "reject client");
      reactor.accept_sessions();
      check(counts.snapshot().rejected_total == 1, "TCP capacity reject");
      opt.max_sessions = 1024;
      Fd timed(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
      check(
          connect(timed.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0,
          "timeout client");
      reactor.accept_sessions();
      opt.connect_timeout = std::chrono::milliseconds(0);
      reactor.deadlines();
      check(counts.snapshot().timeouts_total == 1 &&
                counts.snapshot().errors_total == 5,
            "timeout not error");
      opt.connect_timeout = std::chrono::seconds(5);
      opt.connect_call = [](int, const sockaddr*, socklen_t) {
        errno = ECONNREFUSED;
        return -1;
      };
      Fd failed(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
      check(connect(failed.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) ==
                0,
            "failed client");
      reactor.accept_sessions();
      check(counts.snapshot().errors_total == 6 &&
                counts.snapshot().sessions_active == 0,
            "SessionFailure counted once");
      opt.connect_call = {};
      Fd stop(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
      check(
          connect(stop.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0,
          "stop client");
      reactor.accept_sessions();
      check(counts.snapshot().sessions_active == 1,
            "stop active before unwind");
    }
    auto s = counts.snapshot();
    check(s.sessions_active == 0 &&
              s.sessions_created_total == s.sessions_closed_total &&
              s.errors_total == 6,
          "destructor cleanup counted once");
  }
};
}  // namespace
}  // namespace l4lb::net

int main() {
  auto count_fds = [] {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator{});
  };
  auto before = count_fds();
  try {
    l4lb::net::ReactorTestAccess::run();
    if (count_fds() != before)
      throw std::runtime_error("metrics reactor fd leak");
    std::cout << "PASS TCP actual "
                 "send/lifecycle/accept/errors/timeouts/cleanup points\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
