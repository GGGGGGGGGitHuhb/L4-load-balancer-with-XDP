#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <iostream>
#include <stdexcept>
#include <thread>

#include "control/HealthSelection.h"
using namespace l4lb;
using namespace l4lb::health;
using namespace std::chrono_literals;

namespace {
int checks = 0;

void check(bool ok, const char* what) {
  ++checks;
  if (!ok) throw std::runtime_error(what);
}

std::size_t fds() {
  auto* d = opendir("/proc/self/fd");
  if (!d) throw std::runtime_error("opendir");
  std::size_t n = 0;
  while (readdir(d)) ++n;
  closedir(d);
  return n;
}

Endpoint endpoint{{127, 0, 0, 1}, 1};

void state_tests() {
  for (unsigned bits = 0; bits < 4096; ++bits) {
    HealthState state;
    HealthStatus expected = HealthStatus::kUnknown;
    unsigned good = 0, bad = 0;
    for (unsigned j = 0; j < 12; ++j) {
      bool success = bits & (1u << j);
      if (success) {
        ++good;
        bad = 0;
        if (good >= 2) expected = HealthStatus::kHealthy;
      } else {
        ++bad;
        good = 0;
        if (bad >= 3) expected = HealthStatus::kUnhealthy;
      }
      auto before = state.status;
      check(state.applyProbeResult(success) == (before != expected),
            "state transition");
      check(state.status == expected && state.successes == std::min(good, 2u) &&
                state.failures == std::min(bad, 3u),
            "state oracle");
    }
    HealthState other;
    check(other.status == HealthStatus::kUnknown && other.successes == 0,
          "restart/isolation");
  }
  auto scheduler = createBackendScheduler(SchedulerKind::kRoundRobin, 3);
  check(!selectEligibleBackendIndex(*scheduler, {false, false, false}),
        "all bad");
  check(selectEligibleBackendIndex(*scheduler, {true, false, true}) == 0,
        "no advancement");
  check(selectEligibleBackendIndex(*scheduler, {true, false, true}) == 2,
        "skip");
  check(selectEligibleBackendIndex(*scheduler, {false, true, true}) == 1,
        "recovery cursor");
  check(selectEligibleBackendIndex(*scheduler, {true, true, true}) == 2,
        "normal cursor");
  Config c;
  c.backends = {endpoint};
  c.healthCheck = static_cast<HealthCheck>(99);
  bool rejected = false;
  try {
    HealthSelection invalid(c);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "unknown internal enum");
  c.healthCheck = HealthCheck::kOff;
  auto count = fds();
  {
    HealthSelection off(c);
    check(!off.enabled() && off.selectBackend() == endpoint && fds() == count,
          "off creates no checker fd");
  }
}

void config_tests() {
  std::string base = "listen=127.0.0.1:3000\nbackend=127.0.0.1:3001\n";
  for (auto value : {"", "off", "tcp_connect"}) {
    std::string field = std::string(value).empty()
                            ? ""
                            : std::string("health_check=") + value + "\n";
    for (bool first : {false, true}) {
      auto r = parseConfig(first ? field + base : base + field);
      auto* c = std::get_if<Config>(&r);
      check(c && c->healthCheck == (std::string(value) == "tcp_connect"
                                        ? HealthCheck::kTcpConnect
                                        : HealthCheck::kOff),
            "config health valid/order/default");
    }
  }
  for (auto field : {"health_check=", "health_check=OFF",
                     "health_check=TCP_CONNECT", "health_check=udp",
                     "Health_check=off", "health_check=bad\nprotocol=bad"}) {
    auto r = parseConfig(std::string(field) + "\n" + base);
    auto* e = std::get_if<ConfigError>(&r);
    check(e && e->line == 1, "config health first error");
  }
  for (auto second : {"off", "tcp_connect", "invalid"}) {
    auto r = parseConfig(std::string("health_check=off\nhealth_check=") +
                         second + "\n" + base);
    auto* e = std::get_if<ConfigError>(&r);
    check(e && e->line == 2, "config health duplicate");
  }
}

void injected_tests() {
  auto count = fds();
  Clock::time_point time{};
  TcpHealthCheckOptions opt;
  opt.now = [&] { return time; };
  opt.connectCall = [](int fd, const sockaddr*, socklen_t) {
    check(
        (fcntl(fd, F_GETFL) & O_NONBLOCK) && (fcntl(fd, F_GETFD) & FD_CLOEXEC),
        "probe flags");
    errno = EINPROGRESS;
    return -1;
  };
  std::uint64_t latest = 0, event = 0;
  int first_fd = -1, last_fd = -1, error_reads = 0;
  opt.epollControlCall = [&](int, int, int fd, epoll_event* e) {
    latest = e->data.u64;
    last_fd = fd;
    if (first_fd < 0) first_fd = fd;
    return 0;
  };
  opt.epollPollCall = [&](int, epoll_event* e, int) {
    if (!event) return 0;
    e[0].data.u64 = event;
    e[1] = e[0];
    return 2;
  };
  opt.socketErrorCall = [&](int, int* error) {
    ++error_reads;
    *error = 0;
    return 0;
  };
  {
    TcpHealthChecker c({endpoint}, opt);
    c.pollHealthProbes();
    auto old = latest;
    check(c.activeProbeCount() == 1, "pending started");
    c.pollHealthProbes();
    check(latest == old && c.activeProbeCount() == 1, "no overlap same tick");
    time += 1000ms;
    event = old;
    c.pollHealthProbes();
    check(c.backendState(0).failures == 1 && error_reads == 0 &&
              c.activeProbeCount() == 0,
          "exact timeout beats late success");
    c.pollHealthProbes();
    check(c.backendState(0).failures == 1, "duplicate timeout no completion");
    time += 1000ms;
    c.pollHealthProbes();
    check(latest != old && last_fd == first_fd, "real fd reuse new token");
    c.pollHealthProbes();
    check(error_reads == 0 && c.activeProbeCount() == 1, "stale token ignored");
    event = latest;
    c.pollHealthProbes();
    check(c.backendState(0).successes == 1 && c.backendState(0).failures == 0 &&
              error_reads == 1 && !c.activeProbeCount(),
          "duplicate event counts once");
    time += 10s;
    event = 0;
    c.pollHealthProbes();
    check(c.activeProbeCount() == 1, "no catchup storm");
  }
  check(fds() == count, "cancel reclaims pending fd epoll");
  {
    TcpHealthChecker many(std::vector<Endpoint>(256, endpoint), opt);
    event = 0;
    many.pollHealthProbes();
    check(many.activeProbeCount() == 256, "256 upper bound");
    many.pollHealthProbes();
    check(many.activeProbeCount() == 256, "256 no overlap");
  }
  check(fds() == count, "256 cancel cleanup");
  for (int failure : {EMFILE, ENFILE, ENOMEM}) {
    TcpHealthCheckOptions resource;
    resource.now = [&] { return time; };
    resource.socketCall = [=] {
      errno = failure;
      return -1;
    };
    std::string reason;
    int observed = 0;
    TcpHealthChecker c({endpoint}, resource);
    c.setHealthChangeCallback([&](const HealthChange& e) {
      reason = e.reason;
      observed = e.error;
    });
    for (int n = 0; n < 3; ++n) {
      c.pollHealthProbes();
      time += 1s;
    }
    check(c.backendState(0).status == HealthStatus::kUnhealthy &&
              reason == "local_error" && observed == failure,
          "resource classification");
  }
  opt.epollControlCall = [](int, int, int, epoll_event*) {
    errno = ENOMEM;
    return -1;
  };
  {
    std::string reason;
    TcpHealthChecker c({endpoint}, opt);
    c.setHealthChangeCallback(
        [&](const HealthChange& e) { reason = e.reason; });
    for (int n = 0; n < 3; ++n) {
      c.pollHealthProbes();
      time += 1s;
      check(!c.activeProbeCount(), "registration failure cleanup");
    }
    check(reason == "local_error", "registration classification");
  }
  opt.epollPollCall = [](int, epoll_event*, int) {
    errno = EIO;
    return -1;
  };
  {
    TcpHealthChecker c({endpoint}, opt);
    bool threw = false;
    try {
      c.pollHealthProbes();
    } catch (const std::system_error&) {
      threw = true;
    }
    check(threw, "checker failure fatal");
  }
  check(fds() == count, "all injected resource cleanup");
  std::cout << "PASS controlled pending exact deadline, duplicate/old token "
               "with real fd reuse, local errors, 256, cancel; timeout is "
               "injected, not natural network\n";
}

void real_tests() {
  auto count = fds();
  {
    net::Fd listener(
        socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(bind(listener.fd(), reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) == 0 &&
              listen(listener.fd(), 32) == 0,
          "real listen");
    socklen_t n = sizeof(addr);
    getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&addr), &n);
    Endpoint target{{127, 0, 0, 1}, ntohs(addr.sin_port)};
    TcpHealthCheckOptions opt;
    opt.interval = 5ms;
    opt.timeout = 200ms;
    TcpHealthChecker c({target}, opt);
    auto wait = [&](HealthStatus expected) {
      auto deadline = Clock::now() + 2s;
      while (Clock::now() < deadline && c.backendState(0).status != expected) {
        c.pollHealthProbes();
        std::this_thread::sleep_for(1ms);
      }
      check(c.backendState(0).status == expected,
            "real checker expected state");
    };
    wait(HealthStatus::kHealthy);
    unsigned empty = 0;
    for (;;) {
      net::Fd accepted(accept4(listener.fd(), nullptr, nullptr,
                               SOCK_NONBLOCK | SOCK_CLOEXEC));
      if (accepted.fd() < 0) break;
      char byte;
      check(recv(accepted.fd(), &byte, 1, 0) == 0, "probe sends no payload");
      ++empty;
    }
    check(empty == 2, "two empty connections");
    listener.closeFd();
    wait(HealthStatus::kUnhealthy);
    check(c.backendState(0).failures == 3, "real refused three failures");
  }
  check(fds() == count, "real checker cleanup");
  std::cout << "PASS real TCP connect/refused with no probe payload\n";
}
}  // namespace

int main(int argc, char**) {
  try {
    if (argc > 1)
      real_tests();
    else {
      state_tests();
      config_tests();
      injected_tests();
    }
    std::cout << "PASS health checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
