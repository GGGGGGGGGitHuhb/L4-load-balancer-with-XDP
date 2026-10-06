#include <filesystem>
#include <iostream>

#include "metrics/Metrics.h"
#include "net/TcpReactor.cpp"

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
  static void runTcpReactor() {
    metrics::MetricsCollector counts(Protocol::kTcp, 1);
    TcpReactorCallbacks cb;
    cb.setStatisticsCallback([&](StatEvent e) {
      check(counts.recordStatEvent(e), "TCP model lifecycle");
    });
    cb.setReadyCallback([] {});
    cb.setSessionCallback([](const SessionEvent&) {});
    cb.setDiagnosticCallback([](const std::string&, int) {});
    Fd backend(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    auto a = makeSocketAddress({{127, 0, 0, 1}, 0});
    check(bind(backend.fd(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 &&
              listen(backend.fd(), 8) == 0,
          "backend");
    cb.setBackendSelector([&] { return local(backend.fd()); });
    TcpReactorOptions opt;
    opt.sendCall = [](int fd, const void* data, std::size_t n, int flags) {
      return send(fd, data, std::min(n, std::size_t(7)), flags);
    };
    {
      TcpReactor reactor({{127, 0, 0, 1}, 0}, cb, opt);
      Fd front(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
      a = makeSocketAddress(local(reactor.listenerFd_.fd()));
      check(
          connect(front.fd(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0,
          "client connect");
      reactor.acceptClientSessions();
      check(counts.snapshot().sessionsCreatedTotal == 1 &&
                counts.snapshot().sessionsActive == 1,
            "real created point");
      auto& s = *reactor.sessions_.begin()->second;
      s.connecting = false;
      std::memcpy(s.pending[0].writable(), "abcdefghijk", 11);
      s.pending[0].commitWrittenBytes(11);
      std::size_t budget = 7;
      reactor.writeBufferedDirection(s, 0, budget);
      check(counts.snapshot().bytesC2bTotal == 7 &&
                counts.snapshot().sessionsActive == 1,
            "TCP bytes must be immediate before close");
      budget = 4;
      reactor.writeBufferedDirection(s, 0, budget);
      check(counts.snapshot().bytesC2bTotal == 11, "TCP partial send totals");
      auto id = s.id;
      reactor.closeSession(id, "drained", 0);
      reactor.closeSession(id, "drained", 0);
      check(counts.snapshot().bytesC2bTotal == 11 &&
                counts.snapshot().sessionsClosedTotal == 1 &&
                counts.snapshot().sessionsActive == 0,
            "close no byte duplicate");
      int attempts = 0;
      opt.acceptCall = [&](int, sockaddr*, socklen_t*, int) {
        errno = attempts++ < 3 ? ECONNABORTED : EAGAIN;
        return -1;
      };
      reactor.acceptClientSessions();
      check(counts.snapshot().errorsTotal == 3,
            "each ignored accept failure counts");
      attempts = 0;
      unsigned logged = 0;
      cb.setDiagnosticCallback([&](const std::string&, int) { ++logged; });
      opt.acceptCall = [&](int, sockaddr*, socklen_t*, int) {
        errno = EMFILE;
        ++attempts;
        return -1;
      };
      reactor.acceptClientSessions();
      reactor.listenerRetryTime_ = Clock::time_point{};
      reactor.processSessionDeadlines();
      reactor.acceptClientSessions();
      check(counts.snapshot().errorsTotal == 5 && logged == 1,
            "accept resource errors before suppression");
      opt.acceptCall = {};
      reactor.listenerRetryTime_ = Clock::time_point{};
      reactor.processSessionDeadlines();
      opt.maxSessions = 0;
      Fd rejected(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
      check(connect(rejected.fd(), reinterpret_cast<sockaddr*>(&a),
                    sizeof(a)) == 0,
            "reject client");
      reactor.acceptClientSessions();
      check(counts.snapshot().rejectedTotal == 1, "TCP capacity reject");
      opt.maxSessions = 1024;
      Fd timed(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
      check(
          connect(timed.fd(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0,
          "timeout client");
      reactor.acceptClientSessions();
      opt.connectTimeout = std::chrono::milliseconds(0);
      reactor.processSessionDeadlines();
      check(counts.snapshot().timeoutsTotal == 1 &&
                counts.snapshot().errorsTotal == 5,
            "timeout not error");
      opt.connectTimeout = std::chrono::seconds(5);
      opt.connectCall = [](int, const sockaddr*, socklen_t) {
        errno = ECONNREFUSED;
        return -1;
      };
      Fd failed(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
      check(
          connect(failed.fd(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0,
          "failed client");
      reactor.acceptClientSessions();
      check(counts.snapshot().errorsTotal == 6 &&
                counts.snapshot().sessionsActive == 0,
            "SessionFailure counted once");
      opt.connectCall = {};
      Fd stop(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
      check(connect(stop.fd(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0,
            "stop client");
      reactor.acceptClientSessions();
      check(counts.snapshot().sessionsActive == 1, "stop active before unwind");
    }
    auto s = counts.snapshot();
    check(s.sessionsActive == 0 &&
              s.sessionsCreatedTotal == s.sessionsClosedTotal &&
              s.errorsTotal == 6,
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
    l4lb::net::ReactorTestAccess::runTcpReactor();
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
