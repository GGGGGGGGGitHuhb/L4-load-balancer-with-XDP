#include "net/UdpReactor.h"

#include <arpa/inet.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "net/Fd.h"

namespace l4lb::net {
namespace {
using UClock = UdpIdleDeadline::Clock;

[[noreturn]] void throwUdpSystemError(const char* operation, int error = errno) {
  throw std::system_error(error, std::generic_category(), operation);
}

sockaddr_in makeUdpSocketAddress(const Endpoint& endpoint) {
  sockaddr_in socketAddress{};
  socketAddress.sin_family = AF_INET;
  socketAddress.sin_port = htons(endpoint.port);
  std::memcpy(&socketAddress.sin_addr, endpoint.address.data(), 4);
  return socketAddress;
}

bool isUnicastAddress(const std::array<std::uint8_t, 4>& addressBytes) {
  return addressBytes != std::array<std::uint8_t, 4>{} &&
         addressBytes != std::array<std::uint8_t, 4>{255, 255, 255, 255} &&
         !(addressBytes[0] >= 224 && addressBytes[0] <= 239);
}

bool isResourcePressure(int socketError) {
  return socketError == EAGAIN || socketError == EWOULDBLOCK || socketError == ENOBUFS ||
         socketError == ENOMEM || socketError == EMSGSIZE;
}

bool isSharedSocketError(int socketError) {
  return isResourcePressure(socketError) || socketError == ECONNREFUSED ||
         socketError == ECONNRESET || socketError == ENETUNREACH || socketError == EHOSTUNREACH ||
         socketError == EACCES;
}

class UdpSignalMask {
 public:
  UdpSignalMask() {
    sigemptyset(&blockedSignals_);
    sigaddset(&blockedSignals_, SIGINT);
    sigaddset(&blockedSignals_, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &blockedSignals_, &previousSignalMask_) < 0)
      throwUdpSystemError("sigprocmask");
  }

  ~UdpSignalMask() { sigprocmask(SIG_SETMASK, &previousSignalMask_, nullptr); }

  const sigset_t* blockedSignals() const { return &blockedSignals_; }

 private:
  sigset_t blockedSignals_{}, previousSignalMask_{};
};

struct UdpFlow {
  UdpFlowKey key;
  Endpoint backend;
  Fd fd;
  std::uint64_t token;
  UdpIdleDeadline deadline;
};

class UdpReactor {
  friend struct UdpTestAccess;

 public:
  UdpReactor(const Endpoint& endpoint,
             const UdpReactorCallbacks& callbacks,
             const UdpReactorOptions& options);
  ~UdpReactor() noexcept;

  void closeAllFlows();

  int runUdpEventLoop();

 private:
  void reportStatEvent(StatKind kind, std::uint64_t amount = 1) {
    if (callbacks_.statisticsCallback()) callbacks_.statisticsCallback()({kind, amount});
  }

  UClock::time_point currentTime() const { return options_.now ? options_.now() : UClock::now(); }

  void reportObservation(const char* kind, std::uint64_t token = 0, int fd = -1) {
    if (options_.observationCallback())
      options_.observationCallback()({kind, token, fd, flows_.size(), flowTokensByKey_.size()});
  }

  void reportDiagnostic(const char* kind, int error = 0);

  void registerFlowSocket(int fd, std::uint64_t token);
  bool consumeStopSignals();

  void attemptCloseErrorReport(std::exception_ptr& firstFailure);
  void attemptCloseDiagnosticReport(int detachError, std::exception_ptr& firstFailure);
  void attemptClosedStatReport(std::exception_ptr& firstFailure);
  void attemptClosedObservationReport(const char* reason,
                                      std::uint64_t token,
                                      int fd,
                                      std::exception_ptr& firstFailure);
  void attemptFlowEventReport(std::uint64_t token,
                              const Endpoint& backend,
                              const char* reason,
                              int error,
                              std::exception_ptr& firstFailure);
  void closeFlow(std::uint64_t token, const char* reason, int error = 0);
  void expireIdleFlows();

  int injectedSetupError(const char* name, int fd) {
    return options_.setupError ? options_.setupError(name, fd) : 0;
  }

  std::uint64_t createFlow(const UdpFlowKey& key);
  bool registerFlowIndexesAndSocket(const UdpFlowKey& key,
                                    std::uint64_t token,
                                    std::unique_ptr<UdpFlow>& flow);
  bool parsePacketMetadata(const msghdr& msg, const sockaddr_in& from, UdpFlowKey& key) const;

  ssize_t receiveDatagram(int fd, msghdr& msg) {
    return options_.recvmsgCall ? options_.recvmsgCall(fd, &msg, 0) : recvmsg(fd, &msg, 0);
  }

  bool handleSocketError(int code, bool listener, std::uint64_t token, const char* kind);
  void sendFlowDatagram(UdpFlow& flow, std::size_t size, bool reply);
  void handleListenerRead();
  std::uint64_t resolveFlowTokenForDatagram(const UdpFlowKey& key);
  void handleBackendRead(std::uint64_t token);
  void dispatchFlowEvent(std::uint64_t token, std::uint32_t events);

  Endpoint listenEndpoint_;
  const UdpReactorCallbacks& callbacks_;
  const UdpReactorOptions& options_;

  UdpSignalMask signalMask_;
  Fd signalFd_, epollFd_, listenerFd_;

  std::vector<char> receiveBuffer_;

  std::uint64_t nextFlowToken_ = 3;
  std::unordered_map<UdpFlowKey, std::uint64_t, UdpFlowHash> flowTokensByKey_;
  std::unordered_map<std::uint64_t, std::unique_ptr<UdpFlow>> flows_;

  std::unordered_map<std::string, UClock::time_point> lastDiagnosticTimes_;
};

UdpReactor::UdpReactor(const Endpoint& endpoint,
                       const UdpReactorCallbacks& callbacks,
                       const UdpReactorOptions& options)
    : listenEndpoint_(endpoint),
      callbacks_(callbacks),
      options_(options),
      signalFd_(signalfd(-1, signalMask_.blockedSignals(), SFD_NONBLOCK | SFD_CLOEXEC)),
      epollFd_(epoll_create1(EPOLL_CLOEXEC)),
      listenerFd_(socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)),
      receiveBuffer_(kUdpPayloadLimit) {
  if (signalFd_.fd() < 0) throwUdpSystemError("signalfd");
  if (epollFd_.fd() < 0) throwUdpSystemError("epoll_create1");
  if (listenerFd_.fd() < 0) throwUdpSystemError("UDP listener socket");

  int yes = 1;
  if (setsockopt(listenerFd_.fd(), IPPROTO_IP, IP_PKTINFO, &yes, sizeof(yes)) < 0)
    throwUdpSystemError("IP_PKTINFO");

  auto socketAddress = makeUdpSocketAddress(endpoint);
  if (bind(listenerFd_.fd(), reinterpret_cast<sockaddr*>(&socketAddress), sizeof(socketAddress)) <
      0)
    throwUdpSystemError("UDP bind");

  registerFlowSocket(listenerFd_.fd(), 1);
  registerFlowSocket(signalFd_.fd(), 2);
}

UdpReactor::~UdpReactor() noexcept {
  try {
    closeAllFlows();
  } catch (...) {
  }
}

void UdpReactor::closeAllFlows() {
  std::exception_ptr failure;
  while (!flows_.empty()) {
    try {
      closeFlow(flows_.begin()->first, "service-stop");
    } catch (...) {
      if (!failure) failure = std::current_exception();
    }
  }

  if (failure) std::rethrow_exception(failure);
}

int UdpReactor::runUdpEventLoop() {
  if (callbacks_.readyCallback()) callbacks_.readyCallback()();
  std::array<epoll_event, 128> events{};

  while (!consumeStopSignals()) {
    expireIdleFlows();

    int eventCount = epoll_wait(epollFd_.fd(),
                                events.data(),
                                events.size(),
                                static_cast<int>(options_.pollInterval.count()));
    if (eventCount < 0) {
      if (errno == EINTR) continue;
      throwUdpSystemError("UDP epoll_wait");
    }

    if (consumeStopSignals()) break;
    expireIdleFlows();
    if (callbacks_.maintenanceCallback()) callbacks_.maintenanceCallback()();

    for (int eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
      if (consumeStopSignals()) return 0;
      dispatchFlowEvent(events[eventIndex].data.u64, events[eventIndex].events);
    }
  }
  return 0;
}

void UdpReactor::reportDiagnostic(const char* kind, int error) {
  reportObservation(kind);

  auto time = currentTime();
  auto diagnosticIt = lastDiagnosticTimes_.find(kind);
  if (diagnosticIt != lastDiagnosticTimes_.end() &&
      time - diagnosticIt->second < std::chrono::seconds(1))
    return;

  lastDiagnosticTimes_[kind] = time;
  if (callbacks_.diagnosticCallback()) callbacks_.diagnosticCallback()(kind, error);
}

void UdpReactor::registerFlowSocket(int fd, std::uint64_t token) {
  epoll_event epollEvent{};
  epollEvent.events = EPOLLIN;
  epollEvent.data.u64 = token;
  if (epoll_ctl(epollFd_.fd(), EPOLL_CTL_ADD, fd, &epollEvent) < 0)
    throwUdpSystemError("UDP epoll add");
}

bool UdpReactor::consumeStopSignals() {
  signalfd_siginfo info{};
  auto receivedBytes = read(signalFd_.fd(), &info, sizeof(info));
  if (receivedBytes == sizeof(info)) return true;
  if (receivedBytes < 0 && (errno == EAGAIN || errno == EINTR)) return false;
  throwUdpSystemError("UDP signalfd read", receivedBytes < 0 ? errno : EIO);
}

void UdpReactor::attemptCloseErrorReport(std::exception_ptr& firstFailure) {
  try {
    reportStatEvent(StatKind::kError);
  } catch (...) {
    if (!firstFailure) firstFailure = std::current_exception();
  }
}

void UdpReactor::attemptCloseDiagnosticReport(int detachError, std::exception_ptr& firstFailure) {
  try {
    if (callbacks_.diagnosticCallback())
      callbacks_.diagnosticCallback()("UDP epoll DEL", detachError);
  } catch (...) {
    if (!firstFailure) firstFailure = std::current_exception();
  }
}

void UdpReactor::attemptClosedStatReport(std::exception_ptr& firstFailure) {
  try {
    reportStatEvent(StatKind::kClosed);
  } catch (...) {
    if (!firstFailure) firstFailure = std::current_exception();
  }
}

void UdpReactor::attemptClosedObservationReport(const char* reason,
                                                std::uint64_t token,
                                                int fd,
                                                std::exception_ptr& firstFailure) {
  try {
    reportObservation(reason, token, fd);
  } catch (...) {
    if (!firstFailure) firstFailure = std::current_exception();
  }
}

void UdpReactor::attemptFlowEventReport(std::uint64_t token,
                                        const Endpoint& backend,
                                        const char* reason,
                                        int error,
                                        std::exception_ptr& firstFailure) {
  try {
    if (callbacks_.flowCallback()) callbacks_.flowCallback()({token, backend, reason, error});
  } catch (...) {
    if (!firstFailure) firstFailure = std::current_exception();
  }
}

void UdpReactor::closeFlow(std::uint64_t token, const char* reason, int error) {
  auto flowIt = flows_.find(token);
  if (flowIt == flows_.end()) return;

  auto flow = std::move(flowIt->second);
  flowTokensByKey_.erase(flow->key);
  flows_.erase(flowIt);

  int detachError = 0;
  if (epoll_ctl(epollFd_.fd(), EPOLL_CTL_DEL, flow->fd.fd(), nullptr) < 0 && errno != ENOENT &&
      errno != EBADF)
    detachError = errno;
  int fd = flow->fd.fd();
  flow->fd.closeFd();

  std::exception_ptr failure;

  if (detachError) {
    attemptCloseErrorReport(failure);
    attemptCloseDiagnosticReport(detachError, failure);
  }

  attemptClosedStatReport(failure);
  attemptClosedObservationReport(reason, token, fd, failure);
  attemptFlowEventReport(token, flow->backend, reason, error, failure);

  if (failure) std::rethrow_exception(failure);
}

void UdpReactor::expireIdleFlows() {
  auto time = currentTime();
  for (auto flowIt = flows_.begin(); flowIt != flows_.end();) {
    auto token = flowIt->first;
    bool due = flowIt->second->deadline.expired(time, options_.idleTimeout);
    ++flowIt;
    if (due) {
      reportStatEvent(StatKind::kTimeout);
      closeFlow(token, "idle-timeout");
    }
  }
}

std::uint64_t UdpReactor::createFlow(const UdpFlowKey& key) {
  expireIdleFlows();
  if (flows_.size() >= options_.maxFlows) {
    reportStatEvent(StatKind::kRejected);
    reportDiagnostic("capacity-drop");
    return 0;
  }

  auto selected = callbacks_.backendSelector()();  // 真正异常仍传播。
  if (!selected) {
    reportStatEvent(StatKind::kRejected);
    return 0;
  }
  Endpoint backend = *selected;

  int error = injectedSetupError("socket", -1);
  Fd fd(error ? -1 : socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (fd.fd() < 0) {
    reportStatEvent(StatKind::kError);
    reportDiagnostic("setup-drop", error ? error : errno);
    return 0;
  }

  auto socketAddress = makeUdpSocketAddress(Endpoint{});
  error = injectedSetupError("bind", fd.fd());
  if (error ||
      bind(fd.fd(), reinterpret_cast<sockaddr*>(&socketAddress), sizeof(socketAddress)) < 0) {
    reportStatEvent(StatKind::kError);
    reportDiagnostic("setup-drop", error ? error : errno);
    return 0;
  }

  socketAddress = makeUdpSocketAddress(backend);
  error = injectedSetupError("connect", fd.fd());
  if (error ||
      connect(fd.fd(), reinterpret_cast<sockaddr*>(&socketAddress), sizeof(socketAddress)) < 0) {
    reportStatEvent(StatKind::kError);
    reportDiagnostic("setup-drop", error ? error : errno);
    return 0;
  }

  if (nextFlowToken_ == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("UDP token exhausted");
  auto token = nextFlowToken_++;
  std::unique_ptr<UdpFlow> flow;
  try {
    flow = std::make_unique<UdpFlow>(UdpFlow{key, backend, std::move(fd), token, {currentTime()}});
  } catch (const std::bad_alloc&) {
    reportStatEvent(StatKind::kError);
    reportDiagnostic("setup-drop", ENOMEM);
    return 0;
  }

  error = injectedSetupError("epoll", flow->fd.fd());
  if (error) {
    reportStatEvent(StatKind::kError);
    reportDiagnostic("setup-drop", error);
    return 0;
  }

  if (!registerFlowIndexesAndSocket(key, token, flow)) return 0;

  reportStatEvent(StatKind::kCreated);
  reportObservation("created", token, flows_.at(token)->fd.fd());
  if (callbacks_.flowCallback()) callbacks_.flowCallback()({token, backend, "created"});
  return token;
}

bool UdpReactor::registerFlowIndexesAndSocket(const UdpFlowKey& key,
                                              std::uint64_t token,
                                              std::unique_ptr<UdpFlow>& flow) {
  // 表分配和 epoll 注册均属于事务；失败不留半索引。
  bool indexed = false;
  try {
    flowTokensByKey_.emplace(key, token);
    indexed = true;
    flows_.emplace(token, std::move(flow));
    registerFlowSocket(flows_.at(token)->fd.fd(), token);
  } catch (const std::bad_alloc&) {
    if (indexed) flowTokensByKey_.erase(key);
    flows_.erase(token);
    reportStatEvent(StatKind::kError);
    reportDiagnostic("setup-drop", ENOMEM);
    return false;
  } catch (const std::system_error& setupException) {
    if (indexed) flowTokensByKey_.erase(key);
    flows_.erase(token);
    if (setupException.code().value() == EBADF || setupException.code().value() == EINVAL) throw;
    reportStatEvent(StatKind::kError);
    reportDiagnostic("setup-drop", setupException.code().value());
    return false;
  }

  return true;
}

bool UdpReactor::parsePacketMetadata(const msghdr& msg,
                                     const sockaddr_in& from,
                                     UdpFlowKey& key) const {
  if (msg.msg_namelen < sizeof(sockaddr_in) || from.sin_family != AF_INET || from.sin_port == 0 ||
      (msg.msg_flags & MSG_CTRUNC))
    return false;

  std::memcpy(key.client.address.data(), &from.sin_addr, 4);
  key.client.port = ntohs(from.sin_port);
  if (!isUnicastAddress(key.client.address)) return false;

  int found = 0;
  for (auto* c = CMSG_FIRSTHDR(const_cast<msghdr*>(&msg)); c;
       c = CMSG_NXTHDR(const_cast<msghdr*>(&msg), c)) {
    if (c->cmsg_level != IPPROTO_IP || c->cmsg_type != IP_PKTINFO) continue;
    if (reinterpret_cast<const char*>(c) + CMSG_LEN(sizeof(in_pktinfo)) >
            static_cast<const char*>(msg.msg_control) + msg.msg_controllen ||
        c->cmsg_len != CMSG_LEN(sizeof(in_pktinfo)))
      return false;
    in_pktinfo info{};
    std::memcpy(&info, CMSG_DATA(c), sizeof(info));
    std::memcpy(key.localAddress.data(), &info.ipi_addr, 4);
    if (++found != 1 || !isUnicastAddress(key.localAddress) ||
        info.ipi_addr.s_addr != info.ipi_spec_dst.s_addr)
      return false;
  }

  return found == 1 && (listenEndpoint_.address == std::array<std::uint8_t, 4>{} ||
                        listenEndpoint_.address == key.localAddress);
}

bool UdpReactor::handleSocketError(int code, bool listener, std::uint64_t token, const char* kind) {
  if (code == EBADF || code == ENOTSOCK || code == EINVAL) throwUdpSystemError(kind, code);

  if (listener) {
    if (!isSharedSocketError(code)) throwUdpSystemError(kind, code);
    if (code != EAGAIN && code != EWOULDBLOCK && code != EINTR) reportStatEvent(StatKind::kError);
    reportDiagnostic(kind, code);
    return true;
  }

  if (isResourcePressure(code)) {
    if (code != EAGAIN && code != EWOULDBLOCK && code != EINTR) reportStatEvent(StatKind::kError);
    reportDiagnostic(kind, code);
    return true;
  }

  if (code != EAGAIN && code != EWOULDBLOCK && code != EINTR) reportStatEvent(StatKind::kError);
  closeFlow(token, kind, code);
  return false;
}

void UdpReactor::sendFlowDatagram(UdpFlow& flow, std::size_t size, bool reply) {
  iovec vector{receiveBuffer_.data(), size};
  msghdr msg{};
  msg.msg_iov = &vector;
  msg.msg_iovlen = 1;
  sockaddr_in to{};
  alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(in_pktinfo))> control{};
  int fd = flow.fd.fd();
  if (reply) {
    fd = listenerFd_.fd();
    to = makeUdpSocketAddress(flow.key.client);
    msg.msg_name = &to;
    msg.msg_namelen = sizeof(to);
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();
    auto* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = IPPROTO_IP;
    c->cmsg_type = IP_PKTINFO;
    c->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
    in_pktinfo info{};
    std::memcpy(&info.ipi_spec_dst, flow.key.localAddress.data(), 4);
    std::memcpy(CMSG_DATA(c), &info, sizeof(info));
  }

  for (int attempt = 0; attempt < 4; ++attempt) {
    auto sentBytes = options_.sendmsgCall
                         ? options_.sendmsgCall(fd, &msg, MSG_NOSIGNAL)
                         : (reply ? sendmsg(fd, &msg, MSG_NOSIGNAL)
                                  : send(fd, receiveBuffer_.data(), size, MSG_NOSIGNAL));
    if (sentBytes >= 0) {
      if (static_cast<std::size_t>(sentBytes) == size) {
        reportStatEvent(reply ? StatKind::kBytesB2c : StatKind::kBytesC2b, size);
        reportStatEvent(reply ? StatKind::kDatagramB2c : StatKind::kDatagramC2b);
        flow.deadline.recordSubmission(currentTime());
        reportObservation(reply ? "sent-reply" : "sent-request", flow.token, fd);
      } else {
        reportStatEvent(StatKind::kError);
        reportStatEvent(StatKind::kDropped);
        reportDiagnostic("short-send-drop");
      }
      return;
    }

    if (errno == EINTR) continue;
    reportStatEvent(StatKind::kDropped);
    handleSocketError(errno,
                      reply,
                      flow.token,
                      reply ? "listener-send-drop" : "backend-send-error");
    return;
  }

  reportStatEvent(StatKind::kDropped);
  reportDiagnostic("send-budget-drop");
}

void UdpReactor::handleListenerRead() {
  for (int attempt = 0; attempt < 64; ++attempt) {
    sockaddr_in from{};
    iovec v{receiveBuffer_.data(), options_.receiveSize};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(in_pktinfo))> control{};
    msghdr msg{};
    msg.msg_name = &from;
    msg.msg_namelen = sizeof(from);
    msg.msg_iov = &v;
    msg.msg_iovlen = 1;
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();

    auto receivedBytes = receiveDatagram(listenerFd_.fd(), msg);
    if (receivedBytes < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      handleSocketError(errno, true, 0, "listener-recv-error");
      return;
    }

    if ((msg.msg_flags & MSG_TRUNC) || receivedBytes > static_cast<ssize_t>(kUdpPayloadLimit)) {
      reportStatEvent(StatKind::kDropped);
      reportDiagnostic("listener-truncated");
      continue;
    }

    UdpFlowKey key{};
    if (!parsePacketMetadata(msg, from, key)) {
      reportStatEvent(StatKind::kDropped);
      reportDiagnostic("metadata-drop");
      continue;
    }

    std::uint64_t token = resolveFlowTokenForDatagram(key);

    if (token)
      sendFlowDatagram(*flows_.at(token), static_cast<std::size_t>(receivedBytes), false);
    else
      reportStatEvent(StatKind::kDropped);
  }

  reportObservation("listener-budget");
}

std::uint64_t UdpReactor::resolveFlowTokenForDatagram(const UdpFlowKey& key) {
  auto tokenIt = flowTokensByKey_.find(key);
  std::uint64_t token = tokenIt == flowTokensByKey_.end() ? 0 : tokenIt->second;
  if (token && flows_.at(token)->deadline.expired(currentTime(), options_.idleTimeout)) {
    reportStatEvent(StatKind::kTimeout);
    closeFlow(token, "idle-timeout");
    token = 0;
  }

  if (!token) {
    try {
      token = createFlow(key);
    } catch (...) {
      // 已取得有效数据报但尚未提交；Error由control统一计，不能在这里重复。
      reportStatEvent(StatKind::kDropped);
      throw;
    }
  }

  return token;
}

void UdpReactor::handleBackendRead(std::uint64_t token) {
  for (int attempt = 0; attempt < 64; ++attempt) {
    auto flowIt = flows_.find(token);
    if (flowIt == flows_.end()) return;
    auto& flow = *flowIt->second;
    if (flow.deadline.expired(currentTime(), options_.idleTimeout)) {
      reportStatEvent(StatKind::kTimeout);
      closeFlow(token, "idle-timeout");
      return;
    }

    iovec v{receiveBuffer_.data(), options_.receiveSize};
    msghdr msg{};
    msg.msg_iov = &v;
    msg.msg_iovlen = 1;

    auto receivedBytes = receiveDatagram(flow.fd.fd(), msg);
    if (receivedBytes < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      handleSocketError(errno, false, token, "backend-recv-error");
      return;
    }

    if ((msg.msg_flags & MSG_TRUNC) || receivedBytes > static_cast<ssize_t>(kUdpPayloadLimit)) {
      reportStatEvent(StatKind::kDropped);
      reportDiagnostic("backend-truncated");
      continue;
    }

    sendFlowDatagram(flow, static_cast<std::size_t>(receivedBytes), true);
  }

  reportObservation("backend-budget", token);
}

void UdpReactor::dispatchFlowEvent(std::uint64_t token, std::uint32_t events) {
  if (token == 2) return;

  bool listener = token == 1;
  auto flowIt = flows_.find(token);
  if (!listener && flowIt == flows_.end()) {
    reportObservation("stale-token", token);
    return;
  }

  if (!listener && flowIt->second->deadline.expired(currentTime(), options_.idleTimeout)) {
    reportStatEvent(StatKind::kTimeout);
    closeFlow(token, "idle-timeout");
    return;
  }

  int fd = listener ? listenerFd_.fd() : flowIt->second->fd.fd();
  if (events & EPOLLERR) {
    int code = 0;
    socklen_t socketErrorLength = sizeof(code);
    int socketErrorResult = options_.socketErrorCall
                                ? options_.socketErrorCall(fd, &code)
                                : getsockopt(fd, SOL_SOCKET, SO_ERROR, &code, &socketErrorLength);
    if (socketErrorResult < 0) {
      if (listener) throwUdpSystemError("listener SO_ERROR");
      if (errno == EBADF || errno == ENOTSOCK) throwUdpSystemError("backend SO_ERROR");
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        reportStatEvent(StatKind::kError);
      closeFlow(token, "socket-error-read", errno);
      return;
    }
    reportObservation("epoll-error", token, fd);
    if (code && !handleSocketError(code,
                                   listener,
                                   token,
                                   listener ? "listener-async-error" : "backend-async-error"))
      return;
  }

  if (events & EPOLLHUP) {
    if (listener) throwUdpSystemError("UDP listener HUP", EIO);
    reportStatEvent(StatKind::kError);
    closeFlow(token, "backend-hup");
    return;
  }

  if (events & EPOLLIN) {
    if (listener)
      handleListenerRead();
    else
      handleBackendRead(token);
  }
}

}  // namespace

int runUdpReactor(const Endpoint& endpoint,
                  const UdpReactorCallbacks& callbacks,
                  const UdpReactorOptions& options) {
  if (!options.maxFlows || options.idleTimeout.count() <= 0 || options.pollInterval.count() <= 0 ||
      options.pollInterval.count() > std::numeric_limits<int>::max() || !options.receiveSize ||
      options.receiveSize > kUdpPayloadLimit || !callbacks.backendSelector())
    throw std::invalid_argument("invalid UDP options/callbacks");

  UdpReactor reactor(endpoint, callbacks, options);
  auto result = reactor.runUdpEventLoop();

  reactor.closeAllFlows();
  return result;
}
}  // namespace l4lb::net
