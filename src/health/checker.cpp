#include "health/checker.h"

#include <arpa/inet.h>

#include <array>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace l4lb::health {
namespace {
bool local(int e) {
  return e == EMFILE || e == ENFILE || e == ENOMEM || e == ENOBUFS;
}

void fail(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}
}  // namespace

Checker::Checker(const std::vector<Endpoint>& endpoints,
                 std::function<void(const Change&)> changed, Options options)
    : endpoints_(endpoints),
      changed_(std::move(changed)),
      options_(std::move(options)),
      epoll_(epoll_create1(EPOLL_CLOEXEC)),
      probes_(endpoints.size()) {
  if (endpoints.empty() || endpoints.size() > 256 ||
      options_.interval.count() <= 0 || options_.timeout.count() <= 0)
    throw std::invalid_argument("invalid health checker options");
  if (epoll_.get() < 0) fail("health epoll_create1");
}

Clock::time_point Checker::now() const {
  return options_.now ? options_.now() : Clock::now();
}

std::size_t Checker::active() const {
  std::size_t n = 0;
  for (const auto& p : probes_) n += p.token != 0;
  return n;
}

void Checker::finish(std::size_t i, bool success, const char* reason, int error,
                     Clock::time_point time) {
  auto& p = probes_[i];
  // 先撤销身份，旧就绪事件即使 fd 被复用也无法完成新代次。
  p.token = 0;
  if (p.fd.get() >= 0)
    epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, p.fd.get(), nullptr);
  p.fd.reset();
  p.next = time + options_.interval;
  auto before = p.state.status;
  if (p.state.complete(success) && changed_)
    changed_({i, before, p.state.status, reason, error});
}

void Checker::start(std::size_t i, Clock::time_point time) {
  auto& p = probes_[i];
  p.fd = net::Fd(
      options_.socket_call
          ? options_.socket_call()
          : socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (p.fd.get() < 0) {
    finish(i, false, "local_error", errno, time);
    return;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(endpoints_[i].port);
  const auto& a = endpoints_[i].address;
  addr.sin_addr.s_addr = htonl((unsigned(a[0]) << 24) | (unsigned(a[1]) << 16) |
                               (unsigned(a[2]) << 8) | a[3]);
  int rc =
      options_.connect_call
          ? options_.connect_call(
                p.fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
          : connect(p.fd.get(), reinterpret_cast<sockaddr*>(&addr),
                    sizeof(addr));
  if (rc == 0) {
    finish(i, true, "connected", 0, time);
    return;
  }
  int error = errno;
  if (error != EINPROGRESS) {
    finish(i, false, local(error) ? "local_error" : "connect_error", error,
           time);
    return;
  }
  if (next_token_ == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("health token exhausted");
  p.token = next_token_++;
  p.deadline = time + options_.timeout;
  epoll_event event{};
  event.events = EPOLLOUT | EPOLLERR | EPOLLHUP;
  event.data.u64 = p.token;
  rc = options_.ctl_call
           ? options_.ctl_call(epoll_.get(), EPOLL_CTL_ADD, p.fd.get(), &event)
           : epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, p.fd.get(), &event);
  if (rc < 0) finish(i, false, "local_error", errno, time);
}

void Checker::tick() {
  auto time = now();
  std::array<epoll_event, 256> events{};
  int n = options_.poll_call
              ? options_.poll_call(epoll_.get(), events.data(), events.size())
              : epoll_wait(epoll_.get(), events.data(), events.size(), 0);
  if (n < 0) {
    if (errno == EINTR) return;
    fail("health epoll_wait");
  }
  if (n > static_cast<int>(events.size()))
    throw std::logic_error("health invalid event count");
  for (std::size_t i = 0; i < probes_.size(); ++i) {
    auto& p = probes_[i];
    if (p.token && time >= p.deadline) {
      finish(i, false, "timeout", ETIMEDOUT, time);
      continue;
    }
    if (p.token) {
      for (int j = 0; j < n; ++j) {
        if (events[j].data.u64 != p.token) continue;
        int error = 0;
        socklen_t length = sizeof(error);
        int rc = options_.error_call ? options_.error_call(p.fd.get(), &error)
                                     : getsockopt(p.fd.get(), SOL_SOCKET,
                                                  SO_ERROR, &error, &length);
        if (rc < 0) {
          finish(i, false, "local_error", errno, time);
        } else
          finish(i, error == 0,
                 error == 0 ? "connected"
                            : (local(error) ? "local_error" : "connect_error"),
                 error, time);
        break;
      }
    } else if (time >= p.next)
      start(i, time);
  }
}
}  // namespace l4lb::health
