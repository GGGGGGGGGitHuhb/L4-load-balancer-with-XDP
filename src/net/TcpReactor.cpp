#include "net/TcpReactor.h"

#include <arpa/inet.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <exception>
#include <limits>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "net/Fd.h"

namespace l4lb::net {
namespace {
using namespace std::chrono_literals;

[[noreturn]] void throwTcpSystemError(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

sockaddr_in makeSocketAddress(const Endpoint& endpoint) {
  sockaddr_in result{};
  result.sin_family = AF_INET;
  result.sin_port = htons(endpoint.port);
  std::memcpy(&result.sin_addr, endpoint.address.data(), 4);
  return result;
}

/** mask owner 在所有异常退出路径恢复原状态。 */
class SignalMask {
 public:
  SignalMask() {
    sigemptyset(&blockedSignals_);
    sigaddset(&blockedSignals_, SIGINT);
    sigaddset(&blockedSignals_, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &blockedSignals_, &previousSignalMask_) < 0)
      throwTcpSystemError("sigprocmask");
  }

  ~SignalMask() { sigprocmask(SIG_SETMASK, &previousSignalMask_, nullptr); }

  const sigset_t* blockedSignals() const { return &blockedSignals_; }

 private:
  sigset_t blockedSignals_{}, previousSignalMask_{};
};

struct EndpointState {
  Fd fd;
  std::uint64_t token = 0;
  bool registered = false, eof = false, shutdown = false, paused = false;
  std::uint32_t interest = 0;
  Clock::time_point deferred{};
};

struct Session {
  std::uint64_t id;
  Endpoint backend;
  EndpointState ends[2];
  TcpPendingBuffer pending[2];  // pending[i] 由源 i 读入，向 1-i 发送。
  bool connecting = true;
  TcpIdleDeadline deadline{Clock::now()};
  std::uint64_t sent[2]{};
};

struct SessionFailure {
  std::string operation;
  int error;
};

class TcpReactor {
  friend struct ReactorTestAccess;

 public:
  TcpReactor(const Endpoint& endpoint,
             const TcpReactorCallbacks& callbacks,
             const TcpReactorOptions& options);
  ~TcpReactor() noexcept;

  int runTcpEventLoop();

 private:
  void dispatchTcpReadyEvents(const std::array<epoll_event, 128>& events, int eventCount);

  void reportStatEvent(StatKind kind, std::uint64_t amount = 1) {
    if (callbacks_.statisticsCallback()) callbacks_.statisticsCallback()({kind, amount});
  }

  void reportObservation(const char* kind,
                         const Session* session = nullptr,
                         int side = -1,
                         std::uint64_t value = 0) {
    if (options_.observationCallback())
      options_.observationCallback()(
          {kind, session ? session->id : 0, side, value, sessions_.size(), tokens_.size()});
  }

  bool consumeStopSignals();
  void closeAllSessions(const std::string& reason);
  void advanceStopDrain();

  void updateEpollRegistration(int operation, int fd, std::uint64_t token, std::uint32_t mask);
  int detachEndpointFromEpoll(EndpointState& end) noexcept;
  void removeEndpoint(EndpointState& end);

  void attemptCloseErrorReport(std::exception_ptr& firstFailure);
  void attemptCloseDiagnosticReport(int code, std::exception_ptr& firstFailure);
  void attemptSessionEventReport(const Session& session,
                                 const std::string& reason,
                                 int error,
                                 std::exception_ptr& firstFailure);
  void attemptClosedStatReport(std::exception_ptr& firstFailure);
  void attemptClosedObservationReport(std::exception_ptr& firstFailure);
  void closeSession(std::uint64_t id, const std::string& reason, int error);

  int readSocketError(int fd);
  void acceptClientSessions();
  void connectSessionBackend(Session& session, std::uint64_t id);
  void updateSessionInterests(Session& session);

  void writeBufferedDirection(Session& session, int source, std::size_t& budget);
  void readSessionDirection(Session& session, int source);
  bool advanceSessionIo(Session& session);

  void dispatchEndpointEvent(std::uint64_t token, std::uint32_t events);
  void processSessionDeadlines();

  const TcpReactorCallbacks& callbacks_;
  const TcpReactorOptions& options_;

  SignalMask signalMask_;
  Fd signalFd_, epollFd_, listenerFd_;

  EndpointTokens tokens_;
  std::unordered_map<std::uint64_t, std::unique_ptr<Session>> sessions_;
  std::uint64_t nextSessionId_ = 1;

  Clock::time_point listenerRetryTime_{};

  Clock::time_point drainDeadline_{};
  std::string stopReason_ = "service-stop-deadline";
  bool listenerDeferred_ = false, resourceWarning_ = false, stopRequested_ = false,
       draining_ = false;
};

TcpReactor::TcpReactor(const Endpoint& endpoint,
                       const TcpReactorCallbacks& callbacks,
                       const TcpReactorOptions& options)
    : callbacks_(callbacks),
      options_(options),
      signalFd_(signalfd(-1, signalMask_.blockedSignals(), SFD_NONBLOCK | SFD_CLOEXEC)),
      epollFd_(epoll_create1(EPOLL_CLOEXEC)),
      listenerFd_(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) {
  if (signalFd_.fd() < 0) throwTcpSystemError("signalfd");
  if (epollFd_.fd() < 0) throwTcpSystemError("epoll_create1");
  if (listenerFd_.fd() < 0) throwTcpSystemError("socket listener");

  int yes = 1;
  if (setsockopt(listenerFd_.fd(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
    throwTcpSystemError("setsockopt SO_REUSEADDR");

  auto socketAddress = makeSocketAddress(endpoint);
  if (bind(listenerFd_.fd(), reinterpret_cast<sockaddr*>(&socketAddress), sizeof(socketAddress)) <
      0)
    throwTcpSystemError("bind");
  if (listen(listenerFd_.fd(), 128) < 0) throwTcpSystemError("listen");

  updateEpollRegistration(EPOLL_CTL_ADD, listenerFd_.fd(), 1, EPOLLIN);
  updateEpollRegistration(EPOLL_CTL_ADD, signalFd_.fd(), 2, EPOLLIN);
}

TcpReactor::~TcpReactor() noexcept {
  try {
    closeAllSessions("service-error");
  } catch (...) {
  }
}

int TcpReactor::runTcpEventLoop() {
  callbacks_.readyCallback()();
  std::array<epoll_event, 128> events{};

  for (;;) {
    consumeStopSignals();
    if (stopRequested_) {
      closeAllSessions(stopReason_);
      return 0;
    }
    if (draining_) {
      advanceStopDrain();
      if (sessions_.empty()) return 0;
    } else {
      processSessionDeadlines();
    }

    int pollTimeoutMs = 100;
    if (draining_) {
      auto left =
          std::chrono::ceil<std::chrono::milliseconds>(drainDeadline_ - Clock::now()).count();
      pollTimeoutMs = static_cast<int>(std::clamp<std::int64_t>(left, 0, 100));
    }
    reportObservation("poll", nullptr, -1, pollTimeoutMs);
    // 等待事件
    int eventCount = epoll_wait(epollFd_.fd(), events.data(), events.size(), pollTimeoutMs);
    if (eventCount < 0) {
      if (errno == EINTR) continue;
      throwTcpSystemError("epoll_wait");
    }

    consumeStopSignals();
    if (stopRequested_) continue;
    if (!draining_) {
      processSessionDeadlines();
      if (!draining_ && callbacks_.maintenanceCallback()) callbacks_.maintenanceCallback()();
    }

    dispatchTcpReadyEvents(events, eventCount);
  }
}

void TcpReactor::dispatchTcpReadyEvents(const std::array<epoll_event, 128>& events,
                                        int eventCount) {
  for (int eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
    consumeStopSignals();
    if (stopRequested_) break;
    auto token = events[eventIndex].data.u64;
    if (token == 1) {
      if (draining_) continue;
      if (events[eventIndex].events & (EPOLLERR | EPOLLHUP)) {
        errno = EIO;
        throwTcpSystemError("listener epoll");
      }
      // 接受客户端连接
      if (Clock::now() >= listenerRetryTime_) acceptClientSessions();
    } else if (token != 2) {
      dispatchEndpointEvent(token, events[eventIndex].events);
    }
  }
}

bool TcpReactor::consumeStopSignals() {
  if (stopRequested_) return true;

  signalfd_siginfo info{};
  for (;;) {
    auto count = read(signalFd_.fd(), &info, sizeof(info));
    if (count == sizeof(info)) {
      if (draining_) {
        stopRequested_ = true;
        stopReason_ = "service-stop-forced";
        reportObservation("stop-forced");
        return true;
      }

      draining_ = true;
      drainDeadline_ = Clock::now() + options_.drainTimeout;

      // Only invalidate the listener here: callers may hold a Session&.
      // Session removal is deferred to the safe outer dispatch boundary.
      int detachError = 0;
      if (!listenerDeferred_ &&
          epoll_ctl(epollFd_.fd(), EPOLL_CTL_DEL, listenerFd_.fd(), nullptr) < 0)
        detachError = errno;
      listenerFd_.closeFd();
      listenerDeferred_ = false;

      StopEvent event;
      event.deadlineNs =
          std::chrono::duration_cast<std::chrono::nanoseconds>(drainDeadline_.time_since_epoch())
              .count();
      for (auto& [id, session] : sessions_)
        for (int side = 0; side < 2; ++side) event.pending[side] += session->pending[side].size();

      reportObservation("draining");
      if (callbacks_.stoppingCallback()) callbacks_.stoppingCallback()(event);
      if (detachError && callbacks_.diagnosticCallback())
        callbacks_.diagnosticCallback()("stop listener DEL", detachError);
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && errno == EAGAIN) break;
    if (count < 0) throwTcpSystemError("read signalfd");
    errno = EIO;
    throwTcpSystemError("read signalfd");
  }

  if (draining_ && Clock::now() >= drainDeadline_) {
    stopRequested_ = true;
    stopReason_ = "service-stop-deadline";
    reportObservation("stop-deadline");
  }
  return draining_ || stopRequested_;
}

void TcpReactor::closeAllSessions(const std::string& reason) {
  std::exception_ptr failure;
  while (!sessions_.empty()) {
    try {
      closeSession(sessions_.begin()->first, reason, 0);
    } catch (...) {
      if (!failure) failure = std::current_exception();
    }
  }

  if (failure) std::rethrow_exception(failure);
}

void TcpReactor::advanceStopDrain() {
  for (auto sessionIt = sessions_.begin(); sessionIt != sessions_.end();) {
    auto id = sessionIt++->first;
    consumeStopSignals();
    if (stopRequested_) return;

    auto& session = *sessions_.at(id);
    if (session.connecting) {
      closeSession(id, "service-stop-connecting", 0);
      continue;
    }

    for (auto& end : session.ends)
      if (Clock::now() >= end.deferred) end.deferred = {};
    dispatchEndpointEvent(session.ends[0].token, 0);
  }
}

void TcpReactor::updateEpollRegistration(int operation,
                                         int fd,
                                         std::uint64_t token,
                                         std::uint32_t mask) {
  epoll_event event{};
  event.data.u64 = token;
  event.events = mask;
  if (epoll_ctl(epollFd_.fd(), operation, fd, &event) < 0) throwTcpSystemError("epoll_ctl");
}

int TcpReactor::detachEndpointFromEpoll(EndpointState& end) noexcept {
  int error = 0;
  if (end.registered && epoll_ctl(epollFd_.fd(), EPOLL_CTL_DEL, end.fd.fd(), nullptr) < 0 &&
      errno != ENOENT && errno != EBADF)
    error = errno;

  end.registered = false;
  end.interest = 0;
  return error;
}

void TcpReactor::removeEndpoint(EndpointState& end) {
  int error = detachEndpointFromEpoll(end);
  if (error) {
    reportStatEvent(StatKind::kError);
    if (callbacks_.diagnosticCallback()) callbacks_.diagnosticCallback()("epoll_ctl DEL", error);
  }
}

void TcpReactor::attemptCloseErrorReport(std::exception_ptr& firstFailure) {
  try {
    reportStatEvent(StatKind::kError);
  } catch (...) {
    if (!firstFailure) firstFailure = std::current_exception();
  }
}

void TcpReactor::attemptCloseDiagnosticReport(int code, std::exception_ptr& firstFailure) {
  try {
    if (callbacks_.diagnosticCallback()) callbacks_.diagnosticCallback()("epoll_ctl DEL", code);
  } catch (...) {
    if (!firstFailure) firstFailure = std::current_exception();
  }
}

void TcpReactor::attemptSessionEventReport(const Session& session,
                                           const std::string& reason,
                                           int error,
                                           std::exception_ptr& firstFailure) {
  try {
    if (callbacks_.sessionCallback())
      callbacks_.sessionCallback()(
          {session.id, session.backend, false, reason, error, {session.sent[0], session.sent[1]}});
  } catch (...) {
    if (!firstFailure) firstFailure = std::current_exception();
  }
}

void TcpReactor::attemptClosedStatReport(std::exception_ptr& firstFailure) {
  try {
    reportStatEvent(StatKind::kClosed);
  } catch (...) {
    if (!firstFailure) firstFailure = std::current_exception();
  }
}

void TcpReactor::attemptClosedObservationReport(std::exception_ptr& firstFailure) {
  try {
    reportObservation("closed");
  } catch (...) {
    if (!firstFailure) firstFailure = std::current_exception();
  }
}

void TcpReactor::closeSession(std::uint64_t id, const std::string& reason, int error) {
  auto sessionIt = sessions_.find(id);
  if (sessionIt == sessions_.end()) return;

  auto owner = std::move(sessionIt->second);
  sessions_.erase(sessionIt);
  auto& session = *owner;

  for (auto& end : session.ends) tokens_.unregisterEndpoint(end.token);

  int errors[2]{};
  for (int side = 0; side < 2; ++side) {
    errors[side] = detachEndpointFromEpoll(session.ends[side]);
    session.ends[side].fd.closeFd();
  }

  std::exception_ptr failure;

  for (int code : errors)
    if (code) {
      attemptCloseErrorReport(failure);
      attemptCloseDiagnosticReport(code, failure);
    }

  attemptSessionEventReport(session, reason, error, failure);
  attemptClosedStatReport(failure);
  attemptClosedObservationReport(failure);

  if (failure) std::rethrow_exception(failure);
}

int TcpReactor::readSocketError(int fd) {
  int error = 0;
  socklen_t socketErrorLength = sizeof(error);
  int socketErrorResult = options_.socketErrorCall
                              ? options_.socketErrorCall(fd, &error)
                              : getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &socketErrorLength);
  if (socketErrorResult < 0) throw SessionFailure{"getsockopt SO_ERROR", errno};
  return error;
}

void TcpReactor::acceptClientSessions() {
  for (int sessionAttempt = 0; sessionAttempt < 64 && !consumeStopSignals(); ++sessionAttempt) {
    // 取得新的客户端 socket
    Fd front(
        options_.acceptCall
            ? options_.acceptCall(listenerFd_.fd(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)
            : accept4(listenerFd_.fd(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
    if (front.fd() < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      if (errno == EINTR) {
        --sessionAttempt;
        continue;
      }
      if (errno == ECONNABORTED || errno == EPROTO || errno == ENETDOWN || errno == ENOPROTOOPT ||
          errno == EHOSTDOWN || errno == ENONET || errno == EHOSTUNREACH || errno == EOPNOTSUPP ||
          errno == ENETUNREACH) {
        reportStatEvent(StatKind::kError);
        continue;
      }
      if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
        reportStatEvent(StatKind::kError);
        if (!resourceWarning_) callbacks_.diagnosticCallback()("accept4 resource backoff", errno);
        resourceWarning_ = true;
        updateEpollRegistration(EPOLL_CTL_DEL, listenerFd_.fd(), 1, 0);
        listenerDeferred_ = true;
        listenerRetryTime_ = Clock::now() + 100ms;
        reportObservation("listener-backoff");
        return;
      }
      throwTcpSystemError("accept4");
    }

    resourceWarning_ = false;
    if (sessions_.size() >= options_.maxSessions) {
      reportStatEvent(StatKind::kRejected);
      reportObservation("capacity-reject");
      continue;
    }

    auto backend = callbacks_.backendSelector()();
    if (!backend) {
      reportStatEvent(StatKind::kRejected);
      continue;
    }

    auto sessionOwner = std::make_unique<Session>();
    sessionOwner->id = nextSessionId_++;
    sessionOwner->backend = *backend;
    sessionOwner->ends[0].fd = std::move(front);
    auto id = sessionOwner->id;
    sessions_.emplace(id, std::move(sessionOwner));
    reportStatEvent(StatKind::kCreated);
    auto& session = *sessions_.at(id);
    callbacks_.sessionCallback()({id, session.backend, true, "accepted"});

    try {
      connectSessionBackend(session, id);
    } catch (const SessionFailure& error) {
      reportStatEvent(StatKind::kError);
      closeSession(id, error.operation, error.error);
    }
  }
}

void TcpReactor::connectSessionBackend(Session& session, std::uint64_t id) {
  session.ends[1].fd = Fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (session.ends[1].fd.fd() < 0) throw SessionFailure{"socket backend", errno};

  for (int side = 0; side < 2; ++side)
    session.ends[side].token = tokens_.registerEndpoint(id, side);

  auto socketAddress = makeSocketAddress(session.backend);
  int connectResult = options_.connectCall
                          ? options_.connectCall(session.ends[1].fd.fd(),
                                                 reinterpret_cast<sockaddr*>(&socketAddress),
                                                 sizeof(socketAddress))
                          : connect(session.ends[1].fd.fd(),
                                    reinterpret_cast<sockaddr*>(&socketAddress),
                                    sizeof(socketAddress));
  if (connectResult == 0) {
    session.connecting = false;
    session.deadline.lastIoTime = Clock::now();
    reportObservation("connect-immediate", &session);
  } else if (errno == EINPROGRESS)
    reportObservation("connect-inprogress", &session);
  else
    throw SessionFailure{"connect", errno};

  updateSessionInterests(session);
}

void TcpReactor::updateSessionInterests(Session& session) {
  for (int side = 0; side < 2; ++side) {
    auto& end = session.ends[side];
    std::uint32_t mask = 0;
    if (Clock::now() < end.deferred) {
      removeEndpoint(end);
      continue;
    }

    if (draining_) {
      if (session.pending[1 - side].size()) mask = EPOLLOUT;
    } else if (session.connecting) {
      mask = side == 1 ? EPOLLOUT : EPOLLRDHUP;
    } else {
      if (!end.eof && !end.paused) mask |= EPOLLIN | EPOLLRDHUP;
      if (session.pending[1 - side].size()) mask |= EPOLLOUT;
    }
    if (end.paused) reportObservation("read-suspended", &session, side, mask);
    if (!mask) {
      removeEndpoint(end);
      continue;
    }

    if (end.registered && end.interest == mask) continue;
    epoll_event event{};
    event.data.u64 = end.token;
    event.events = mask;
    if (epoll_ctl(epollFd_.fd(),
                  end.registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD,
                  end.fd.fd(),
                  &event) < 0)
      throw SessionFailure{"epoll_ctl session", errno};
    end.registered = true;
    end.interest = mask;
    reportObservation("interest", &session, side, mask);
  }
}

void TcpReactor::writeBufferedDirection(Session& session, int source, std::size_t& budget) {
  auto& buffer = session.pending[source];
  while (buffer.size() && budget) {
    consumeStopSignals();
    if (stopRequested_) break;

    auto size = std::min(buffer.readableBytes(), budget);
    auto sentBytes =
        options_.sendCall
            ? options_.sendCall(session.ends[1 - source].fd.fd(), buffer.data(), size, MSG_NOSIGNAL)
            : send(session.ends[1 - source].fd.fd(), buffer.data(), size, MSG_NOSIGNAL);
    if (sentBytes < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        reportObservation("send-eagain", &session, source);
        break;
      }
      throw SessionFailure{"send", errno};
    }
    if (sentBytes == 0) throw SessionFailure{"send zero", EIO};
    if (static_cast<std::size_t>(sentBytes) < size)
      reportObservation("short-send", &session, source, sentBytes);

    buffer.consumeReadableBytes(sentBytes);
    budget -= sentBytes;
    session.sent[source] += sentBytes;
    reportStatEvent(source == 0 ? StatKind::kBytesC2b : StatKind::kBytesB2c, sentBytes);
    session.deadline.recordIoProgress(Clock::now(), sentBytes);
  }

  if (!draining_ && session.ends[source].paused && buffer.size() <= kLowWater) {
    session.ends[source].paused = false;
    reportObservation("resume", &session, source, buffer.size());
  }
}

void TcpReactor::readSessionDirection(Session& session, int source) {
  auto& end = session.ends[source];
  auto& buffer = session.pending[source];
  std::size_t budget = kBufferLimit;
  while (!end.eof && !end.paused && budget && !consumeStopSignals()) {
    auto size = std::min(buffer.writableBytes(), budget);
    if (!size) break;
    auto receivedBytes = recv(end.fd.fd(), buffer.writable(), size, 0);
    if (receivedBytes < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      throw SessionFailure{"recv", errno};
    }
    if (!receivedBytes) {
      end.eof = true;
      reportObservation("eof", &session, source);
      break;
    }

    buffer.commitWrittenBytes(receivedBytes);
    budget -= receivedBytes;
    session.deadline.recordIoProgress(Clock::now(), receivedBytes);
    reportObservation("buffer", &session, source, buffer.size());

    if (buffer.size() == kBufferLimit) {
      end.paused = true;
      reportObservation("pause", &session, source, buffer.size());
    }
  }
}

bool TcpReactor::advanceSessionIo(Session& session) {
  std::size_t budgets[2]{kBufferLimit, kBufferLimit};
  for (int side = 0; side < 2; ++side) writeBufferedDirection(session, side, budgets[side]);

  for (int side = 0; side < 2; ++side) readSessionDirection(session, side);

  for (int side = 0; side < 2; ++side) writeBufferedDirection(session, side, budgets[side]);

  if (draining_) return !session.pending[0].size() && !session.pending[1].size();

  for (int side = 0; side < 2; ++side) {
    auto& target = session.ends[1 - side];
    if (session.ends[side].eof && !session.pending[side].size() && !target.shutdown) {
      int shutdownResult;
      do {
        shutdownResult = shutdown(target.fd.fd(), SHUT_WR);
      } while (shutdownResult < 0 && errno == EINTR && !consumeStopSignals());
      if (shutdownResult < 0) throw SessionFailure{"shutdown SHUT_WR", errno};
      target.shutdown = true;
      reportObservation("shutdown", &session, 1 - side);
    }
  }

  return session.ends[0].eof && session.ends[1].eof && session.ends[0].shutdown &&
         session.ends[1].shutdown;
}

void TcpReactor::dispatchEndpointEvent(std::uint64_t token, std::uint32_t events) {
  auto target = tokens_.findEndpoint(token);
  if (!target) {
    reportObservation("stale-token");
    return;
  }

  auto id = target->sessionId;
  int side = target->side;
  auto& session = *sessions_.at(id);
  try {
    if (draining_ && session.connecting) {
      closeSession(id, "service-stop-connecting", 0);
      return;
    }

    bool connectingEvent =
        session.connecting && side == 1 && (events & (EPOLLOUT | EPOLLERR | EPOLLHUP));
    if (connectingEvent || (events & EPOLLERR)) {
      reportObservation("error-event", &session, side, events);
      int error = readSocketError(session.ends[side].fd.fd());
      reportObservation("so-error", &session, side, error);
      if (error) throw SessionFailure{"SO_ERROR", error};
      if (connectingEvent) {
        session.connecting = false;
        session.deadline.lastIoTime = Clock::now();
      }
    }

    auto before = session.deadline.lastIoTime;
    if (!session.connecting && advanceSessionIo(session)) {
      closeSession(id,
                   stopRequested_ ? stopReason_ : (draining_ ? "service-stop-drained" : "drained"),
                   0);
      return;
    }

    // HUP 不可通过事件掩码屏蔽。无进展时暂挂，定时器主动恢复双向 I/O。
    if ((events & (EPOLLHUP | EPOLLRDHUP)) && session.deadline.lastIoTime == before) {
      session.ends[side].deferred = Clock::now() + 100ms;
      reportObservation("hup-defer", &session, side);
      removeEndpoint(session.ends[side]);
    }

    updateSessionInterests(session);
  } catch (const SessionFailure& error) {
    reportStatEvent(StatKind::kError);
    closeSession(id, error.operation, error.error);
  }
}

void TcpReactor::processSessionDeadlines() {
  if (draining_ || stopRequested_) return;

  auto now = Clock::now();
  if (listenerDeferred_ && now >= listenerRetryTime_) {
    updateEpollRegistration(EPOLL_CTL_ADD, listenerFd_.fd(), 1, EPOLLIN);
    listenerDeferred_ = false;
  }

  std::vector<std::uint64_t> ids;
  for (auto& [id, session] : sessions_) ids.push_back(id);

  for (auto id : ids) {
    if (draining_ || stopRequested_) return;
    auto& session = *sessions_.at(id);
    if (session.deadline.expired(
            now,
            session.connecting ? options_.connectTimeout : options_.idleTimeout)) {
      reportStatEvent(StatKind::kTimeout);
      closeSession(id, session.connecting ? "connect-timeout" : "idle-timeout", ETIMEDOUT);
      continue;
    }

    bool retry = false;
    for (auto& end : session.ends) {
      if (end.deferred != Clock::time_point{} && now >= end.deferred) {
        end.deferred = {};
        retry = true;
      }
    }
    if (!retry) continue;

    try {
      reportObservation("hup-retry", &session);
      if (!session.connecting && advanceSessionIo(session)) {
        closeSession(
            id,
            stopRequested_ ? stopReason_ : (draining_ ? "service-stop-drained" : "drained"),
            0);
        continue;
      }
      updateSessionInterests(session);
    } catch (const SessionFailure& error) {
      reportStatEvent(StatKind::kError);
      closeSession(id, error.operation, error.error);
    }
  }
}

}  // namespace

int runTcpReactor(const Endpoint& listen,
                  const TcpReactorCallbacks& callbacks,
                  const TcpReactorOptions& options) {
  if (options.drainTimeout.count() <= 0 ||
      options.drainTimeout.count() > std::numeric_limits<int>::max())
    throw std::invalid_argument("invalid TCP drain timeout");

  TcpReactor reactor(listen, callbacks, options);
  // 进入事件循环
  return reactor.runTcpEventLoop();
}
}  // namespace l4lb::net
