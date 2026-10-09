#include "health/TcpHealthChecker.h"

#include <arpa/inet.h>

#include <array>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace l4lb::health {
namespace {
bool isLocalResourceError(int socketError) {
  return socketError == EMFILE || socketError == ENFILE || socketError == ENOMEM ||
         socketError == ENOBUFS;
}

void throwHealthSystemError(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}
}  // namespace

TcpHealthChecker::TcpHealthChecker(const std::vector<Endpoint>& endpoints,
                                   TcpHealthCheckOptions options)
    : endpoints_(endpoints),
      options_(std::move(options)),
      epollFd_(epoll_create1(EPOLL_CLOEXEC)),
      probes_(endpoints.size()) {
  if (endpoints.empty() || endpoints.size() > 256 || options_.interval.count() <= 0 ||
      options_.timeout.count() <= 0)
    throw std::invalid_argument("invalid health checker options");

  if (epollFd_.fd() < 0) throwHealthSystemError("health epoll_create1");
}

Clock::time_point TcpHealthChecker::currentTime() const {
  return options_.now ? options_.now() : Clock::now();
}

std::size_t TcpHealthChecker::activeProbeCount() const {
  std::size_t activeProbeCount = 0;
  for (const auto& probe : probes_) activeProbeCount += probe.token != 0;
  return activeProbeCount;
}

void TcpHealthChecker::completeBackendProbe(std::size_t backendIndex,
                                            bool success,
                                            const char* reason,
                                            int error,
                                            Clock::time_point time) {
  auto& probe = probes_[backendIndex];
  // 先撤销身份，旧就绪事件即使 fd 被复用也无法完成新代次。
  probe.token = 0;
  if (probe.fd.fd() >= 0) epoll_ctl(epollFd_.fd(), EPOLL_CTL_DEL, probe.fd.fd(), nullptr);
  probe.fd.closeFd();

  probe.nextProbeTime = time + options_.interval;
  auto before = probe.state.status;
  if (probe.state.applyProbeResult(success) && healthChangeCallback_)
    healthChangeCallback_({backendIndex, before, probe.state.status, reason, error});
}

void TcpHealthChecker::startBackendProbe(std::size_t backendIndex, Clock::time_point time) {
  auto& probe = probes_[backendIndex];
  probe.fd =
      net::Fd(options_.socketCall ? options_.socketCall()
                                  : socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (probe.fd.fd() < 0) {
    completeBackendProbe(backendIndex, false, "local_error", errno, time);
    return;
  }

  sockaddr_in socketAddress{};
  socketAddress.sin_family = AF_INET;
  socketAddress.sin_port = htons(endpoints_[backendIndex].port);
  const auto& addressBytes = endpoints_[backendIndex].address;
  socketAddress.sin_addr.s_addr =
      htonl((unsigned(addressBytes[0]) << 24) | (unsigned(addressBytes[1]) << 16) |
            (unsigned(addressBytes[2]) << 8) | addressBytes[3]);
  int probeSetupResult = options_.connectCall
                             ? options_.connectCall(probe.fd.fd(),
                                                    reinterpret_cast<sockaddr*>(&socketAddress),
                                                    sizeof(socketAddress))
                             : connect(probe.fd.fd(),
                                       reinterpret_cast<sockaddr*>(&socketAddress),
                                       sizeof(socketAddress));
  if (probeSetupResult == 0) {
    completeBackendProbe(backendIndex, true, "connected", 0, time);
    return;
  }
  int error = errno;
  if (error != EINPROGRESS) {
    completeBackendProbe(backendIndex,
                         false,
                         isLocalResourceError(error) ? "local_error" : "connect_error",
                         error,
                         time);
    return;
  }

  if (nextProbeToken_ == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("health token exhausted");
  probe.token = nextProbeToken_++;
  probe.probeDeadline = time + options_.timeout;

  epoll_event event{};
  event.events = EPOLLOUT | EPOLLERR | EPOLLHUP;
  event.data.u64 = probe.token;
  probeSetupResult =
      options_.epollControlCall
          ? options_.epollControlCall(epollFd_.fd(), EPOLL_CTL_ADD, probe.fd.fd(), &event)
          : epoll_ctl(epollFd_.fd(), EPOLL_CTL_ADD, probe.fd.fd(), &event);
  if (probeSetupResult < 0) completeBackendProbe(backendIndex, false, "local_error", errno, time);
}

void TcpHealthChecker::pollHealthProbes() {
  auto time = currentTime();
  std::array<epoll_event, 256> events{};
  int eventCount = options_.epollPollCall
                       ? options_.epollPollCall(epollFd_.fd(), events.data(), events.size())
                       : epoll_wait(epollFd_.fd(), events.data(), events.size(), 0);
  if (eventCount < 0) {
    if (errno == EINTR) return;
    throwHealthSystemError("health epoll_wait");
  }
  if (eventCount > static_cast<int>(events.size()))
    throw std::logic_error("health invalid event count");

  for (std::size_t backendIndex = 0; backendIndex < probes_.size(); ++backendIndex) {
    auto& probe = probes_[backendIndex];
    if (probe.token && time >= probe.probeDeadline) {
      completeBackendProbe(backendIndex, false, "timeout", ETIMEDOUT, time);
      continue;
    }

    if (probe.token) {
      for (int eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
        if (events[eventIndex].data.u64 != probe.token) continue;
        int error = 0;
        socklen_t length = sizeof(error);
        int socketErrorResult =
            options_.socketErrorCall
                ? options_.socketErrorCall(probe.fd.fd(), &error)
                : getsockopt(probe.fd.fd(), SOL_SOCKET, SO_ERROR, &error, &length);
        if (socketErrorResult < 0) {
          completeBackendProbe(backendIndex, false, "local_error", errno, time);
        } else
          completeBackendProbe(
              backendIndex,
              error == 0,
              error == 0 ? "connected"
                         : (isLocalResourceError(error) ? "local_error" : "connect_error"),
              error,
              time);
        break;
      }
    } else if (time >= probe.nextProbeTime)
      startBackendProbe(backendIndex, time);
  }
}
}  // namespace l4lb::health
