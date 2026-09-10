#include "net/reactor.h"

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

#include "net/fd.h"

namespace l4lb::net {
namespace {
using namespace std::chrono_literals;

[[noreturn]] void fail(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

sockaddr_in address(const Endpoint& endpoint) {
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
    sigemptyset(&set_);
    sigaddset(&set_, SIGINT);
    sigaddset(&set_, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &set_, &old_) < 0) fail("sigprocmask");
  }

  ~SignalMask() { sigprocmask(SIG_SETMASK, &old_, nullptr); }

  const sigset_t* get() const { return &set_; }

 private:
  sigset_t set_{}, old_{};
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
  Buffer pending[2];  // pending[i] 由源 i 读入，向 1-i 发送。
  bool connecting = true;
  Deadline deadline{Clock::now()};
  std::uint64_t sent[2]{};
};

struct SessionFailure {
  std::string operation;
  int error;
};

class Reactor {
  friend struct ReactorTestAccess;

 public:
  Reactor(const Endpoint& endpoint, const Callbacks& cb, const Options& options)
      : cb_(cb),
        options_(options),
        signals_(signalfd(-1, mask_.get(), SFD_NONBLOCK | SFD_CLOEXEC)),
        epoll_(epoll_create1(EPOLL_CLOEXEC)),
        listener_(
            socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) {
    if (signals_.get() < 0) fail("signalfd");
    if (epoll_.get() < 0) fail("epoll_create1");
    if (listener_.get() < 0) fail("socket listener");
    int yes = 1;
    if (setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &yes,
                   sizeof(yes)) < 0)
      fail("setsockopt SO_REUSEADDR");
    auto addr = address(endpoint);
    if (bind(listener_.get(), reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0)
      fail("bind");
    if (listen(listener_.get(), 128) < 0) fail("listen");
    registration(EPOLL_CTL_ADD, listener_.get(), 1, EPOLLIN);
    registration(EPOLL_CTL_ADD, signals_.get(), 2, EPOLLIN);
  }

  ~Reactor() noexcept {
    try {
      close_all("service-error");
    } catch (...) {
    }
  }

  int loop() {
    cb_.ready();
    std::array<epoll_event, 128> events{};
    for (;;) {
      stopping();
      if (stop_) {
        close_all(stop_reason_);
        return 0;
      }
      if (draining_) {
        drain_tick();
        if (sessions_.empty()) return 0;
      } else {
        deadlines();
      }
      int wait_ms = 100;
      if (draining_) {
        auto left = std::chrono::ceil<std::chrono::milliseconds>(
                        drain_deadline_ - Clock::now())
                        .count();
        wait_ms = static_cast<int>(std::clamp<std::int64_t>(left, 0, 100));
      }
      observe("poll", nullptr, -1, wait_ms);
      int n = epoll_wait(epoll_.get(), events.data(), events.size(), wait_ms);
      if (n < 0) {
        if (errno == EINTR) continue;
        fail("epoll_wait");
      }
      stopping();
      if (stop_) continue;
      if (!draining_) {
        deadlines();
        if (!draining_ && cb_.maintenance) cb_.maintenance();
      }
      for (int i = 0; i < n; ++i) {
        stopping();
        if (stop_) break;
        auto token = events[i].data.u64;
        if (token == 1) {
          if (draining_) continue;
          if (events[i].events & (EPOLLERR | EPOLLHUP)) {
            errno = EIO;
            fail("listener epoll");
          }
          if (Clock::now() >= listener_retry_) accept_sessions();
        } else if (token != 2) {
          dispatch(token, events[i].events);
        }
      }
    }
  }

 private:
  void statistic(StatKind kind, std::uint64_t amount = 1) {
    if (cb_.statistics) cb_.statistics({kind, amount});
  }

  void observe(const char* kind, const Session* s = nullptr, int side = -1,
               std::uint64_t value = 0) {
    if (options_.observe)
      options_.observe(
          {kind, s ? s->id : 0, side, value, sessions_.size(), tokens_.size()});
  }

  bool stopping() {
    if (stop_) return true;
    signalfd_siginfo info{};
    for (;;) {
      auto count = read(signals_.get(), &info, sizeof(info));
      if (count == sizeof(info)) {
        if (draining_) {
          stop_ = true;
          stop_reason_ = "service-stop-forced";
          observe("stop-forced");
          return true;
        }
        draining_ = true;
        drain_deadline_ = Clock::now() + options_.drain_timeout;
        // Only invalidate the listener here: callers may hold a Session&.
        // Session removal is deferred to the safe outer dispatch boundary.
        int del_error = 0;
        if (!listener_deferred_ && epoll_ctl(epoll_.get(), EPOLL_CTL_DEL,
                                             listener_.get(), nullptr) < 0)
          del_error = errno;
        listener_.reset();
        listener_deferred_ = false;
        StopEvent event;
        event.deadline_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                drain_deadline_.time_since_epoch())
                .count();
        for (auto& [id, session] : sessions_)
          for (int side = 0; side < 2; ++side)
            event.pending[side] += session->pending[side].size();
        observe("draining");
        if (cb_.stopping) cb_.stopping(event);
        if (del_error && cb_.diagnostic)
          cb_.diagnostic("stop listener DEL", del_error);
        continue;
      }
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 && errno == EAGAIN) break;
      if (count < 0) fail("read signalfd");
      errno = EIO;
      fail("read signalfd");
    }
    if (draining_ && Clock::now() >= drain_deadline_) {
      stop_ = true;
      stop_reason_ = "service-stop-deadline";
      observe("stop-deadline");
    }
    return draining_ || stop_;
  }

  void close_all(const std::string& reason) {
    std::exception_ptr failure;
    while (!sessions_.empty()) {
      try {
        close(sessions_.begin()->first, reason, 0);
      } catch (...) {
        if (!failure) failure = std::current_exception();
      }
    }
    if (failure) std::rethrow_exception(failure);
  }

  void drain_tick() {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      auto id = it++->first;
      stopping();
      if (stop_) return;
      auto& s = *sessions_.at(id);
      if (s.connecting) {
        close(id, "service-stop-connecting", 0);
        continue;
      }
      for (auto& end : s.ends)
        if (Clock::now() >= end.deferred) end.deferred = {};
      dispatch(s.ends[0].token, 0);
    }
  }

  void registration(int operation, int fd, std::uint64_t token,
                    std::uint32_t mask) {
    epoll_event event{};
    event.data.u64 = token;
    event.events = mask;
    if (epoll_ctl(epoll_.get(), operation, fd, &event) < 0) fail("epoll_ctl");
  }

  int detach(EndpointState& end) noexcept {
    int error = 0;
    if (end.registered &&
        epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, end.fd.get(), nullptr) < 0 &&
        errno != ENOENT && errno != EBADF)
      error = errno;
    end.registered = false;
    end.interest = 0;
    return error;
  }

  void remove(EndpointState& end) {
    int error = detach(end);
    if (error) {
      statistic(StatKind::Error);
      if (cb_.diagnostic) cb_.diagnostic("epoll_ctl DEL", error);
    }
  }

  void close(std::uint64_t id, const std::string& reason, int error) {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    auto owner = std::move(it->second);
    sessions_.erase(it);
    auto& s = *owner;
    for (auto& end : s.ends) tokens_.erase(end.token);
    int errors[2]{};
    for (int side = 0; side < 2; ++side) {
      errors[side] = detach(s.ends[side]);
      s.ends[side].fd.reset();
    }
    std::exception_ptr failure;
    auto attempt = [&](auto action) {
      try {
        action();
      } catch (...) {
        if (!failure) failure = std::current_exception();
      }
    };
    for (int code : errors)
      if (code) {
        attempt([&] { statistic(StatKind::Error); });
        attempt([&] {
          if (cb_.diagnostic) cb_.diagnostic("epoll_ctl DEL", code);
        });
      }
    attempt([&] {
      if (cb_.session)
        cb_.session(
            {s.id, s.backend, false, reason, error, {s.sent[0], s.sent[1]}});
    });
    attempt([&] { statistic(StatKind::Closed); });
    attempt([&] { observe("closed"); });
    if (failure) std::rethrow_exception(failure);
  }

  int socket_error(int fd) {
    int error = 0;
    socklen_t len = sizeof(error);
    int rc = options_.socket_error_call
                 ? options_.socket_error_call(fd, &error)
                 : getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
    if (rc < 0) throw SessionFailure{"getsockopt SO_ERROR", errno};
    return error;
  }

  void accept_sessions() {
    for (int i = 0; i < 64 && !stopping(); ++i) {
      Fd front(options_.accept_call
                   ? options_.accept_call(listener_.get(), nullptr, nullptr,
                                          SOCK_NONBLOCK | SOCK_CLOEXEC)
                   : accept4(listener_.get(), nullptr, nullptr,
                             SOCK_NONBLOCK | SOCK_CLOEXEC));
      if (front.get() < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR) {
          --i;
          continue;
        }
        if (errno == ECONNABORTED || errno == EPROTO || errno == ENETDOWN ||
            errno == ENOPROTOOPT || errno == EHOSTDOWN || errno == ENONET ||
            errno == EHOSTUNREACH || errno == EOPNOTSUPP ||
            errno == ENETUNREACH) {
          statistic(StatKind::Error);
          continue;
        }
        if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
            errno == ENOMEM) {
          statistic(StatKind::Error);
          if (!resource_warning_)
            cb_.diagnostic("accept4 resource backoff", errno);
          resource_warning_ = true;
          registration(EPOLL_CTL_DEL, listener_.get(), 1, 0);
          listener_deferred_ = true;
          listener_retry_ = Clock::now() + 100ms;
          observe("listener-backoff");
          return;
        }
        fail("accept4");
      }
      resource_warning_ = false;
      if (sessions_.size() >= options_.max_sessions) {
        statistic(StatKind::Rejected);
        observe("capacity-reject");
        continue;
      }
      auto backend = cb_.select_backend();
      if (!backend) {
        statistic(StatKind::Rejected);
        continue;
      }
      auto session = std::make_unique<Session>();
      session->id = next_id_++;
      session->backend = *backend;
      session->ends[0].fd = std::move(front);
      auto id = session->id;
      sessions_.emplace(id, std::move(session));
      statistic(StatKind::Created);
      auto& s = *sessions_.at(id);
      cb_.session({id, s.backend, true, "accepted"});
      try {
        s.ends[1].fd =
            Fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (s.ends[1].fd.get() < 0)
          throw SessionFailure{"socket backend", errno};
        for (int side = 0; side < 2; ++side)
          s.ends[side].token = tokens_.add(id, side);
        auto addr = address(s.backend);
        int rc =
            options_.connect_call
                ? options_.connect_call(s.ends[1].fd.get(),
                                        reinterpret_cast<sockaddr*>(&addr),
                                        sizeof(addr))
                : connect(s.ends[1].fd.get(),
                          reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc == 0) {
          s.connecting = false;
          s.deadline.last = Clock::now();
          observe("connect-immediate", &s);
        } else if (errno == EINPROGRESS)
          observe("connect-inprogress", &s);
        else
          throw SessionFailure{"connect", errno};
        update(s);
      } catch (const SessionFailure& error) {
        statistic(StatKind::Error);
        close(id, error.operation, error.error);
      }
    }
  }

  void update(Session& s) {
    for (int side = 0; side < 2; ++side) {
      auto& end = s.ends[side];
      std::uint32_t mask = 0;
      if (Clock::now() < end.deferred) {
        remove(end);
        continue;
      }
      if (draining_) {
        if (s.pending[1 - side].size()) mask = EPOLLOUT;
      } else if (s.connecting) {
        mask = side == 1 ? EPOLLOUT : EPOLLRDHUP;
      } else {
        if (!end.eof && !end.paused) mask |= EPOLLIN | EPOLLRDHUP;
        if (s.pending[1 - side].size()) mask |= EPOLLOUT;
      }
      if (end.paused) observe("read-suspended", &s, side, mask);
      if (!mask) {
        remove(end);
        continue;
      }
      if (end.registered && end.interest == mask) continue;
      epoll_event event{};
      event.data.u64 = end.token;
      event.events = mask;
      if (epoll_ctl(epoll_.get(),
                    end.registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD,
                    end.fd.get(), &event) < 0)
        throw SessionFailure{"epoll_ctl session", errno};
      end.registered = true;
      end.interest = mask;
      observe("interest", &s, side, mask);
    }
  }

  void write_direction(Session& s, int source, std::size_t& budget) {
    auto& buffer = s.pending[source];
    while (buffer.size() && budget) {
      stopping();
      if (stop_) break;
      auto size = std::min(buffer.readable_size(), budget);
      auto n = options_.send_call
                   ? options_.send_call(s.ends[1 - source].fd.get(),
                                        buffer.data(), size, MSG_NOSIGNAL)
                   : send(s.ends[1 - source].fd.get(), buffer.data(), size,
                          MSG_NOSIGNAL);
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          observe("send-eagain", &s, source);
          break;
        }
        throw SessionFailure{"send", errno};
      }
      if (n == 0) throw SessionFailure{"send zero", EIO};
      if (static_cast<std::size_t>(n) < size)
        observe("short-send", &s, source, n);
      buffer.consume(n);
      budget -= n;
      s.sent[source] += n;
      statistic(source == 0 ? StatKind::BytesC2b : StatKind::BytesB2c, n);
      s.deadline.progress(Clock::now(), n);
    }
    if (!draining_ && s.ends[source].paused && buffer.size() <= kLowWater) {
      s.ends[source].paused = false;
      observe("resume", &s, source, buffer.size());
    }
  }

  void read_direction(Session& s, int source) {
    auto& end = s.ends[source];
    auto& buffer = s.pending[source];
    std::size_t budget = kBufferLimit;
    while (!end.eof && !end.paused && budget && !stopping()) {
      auto size = std::min(buffer.writable_size(), budget);
      if (!size) break;
      auto n = recv(end.fd.get(), buffer.writable(), size, 0);
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        throw SessionFailure{"recv", errno};
      }
      if (!n) {
        end.eof = true;
        observe("eof", &s, source);
        break;
      }
      buffer.append(n);
      budget -= n;
      s.deadline.progress(Clock::now(), n);
      observe("buffer", &s, source, buffer.size());
      if (buffer.size() == kBufferLimit) {
        end.paused = true;
        observe("pause", &s, source, buffer.size());
      }
    }
  }

  bool pump(Session& s) {
    std::size_t budgets[2]{kBufferLimit, kBufferLimit};
    for (int side = 0; side < 2; ++side)
      write_direction(s, side, budgets[side]);
    for (int side = 0; side < 2; ++side) read_direction(s, side);
    for (int side = 0; side < 2; ++side)
      write_direction(s, side, budgets[side]);
    if (draining_) return !s.pending[0].size() && !s.pending[1].size();
    for (int side = 0; side < 2; ++side) {
      auto& target = s.ends[1 - side];
      if (s.ends[side].eof && !s.pending[side].size() && !target.shutdown) {
        int rc;
        do {
          rc = shutdown(target.fd.get(), SHUT_WR);
        } while (rc < 0 && errno == EINTR && !stopping());
        if (rc < 0) throw SessionFailure{"shutdown SHUT_WR", errno};
        target.shutdown = true;
        observe("shutdown", &s, 1 - side);
      }
    }
    return s.ends[0].eof && s.ends[1].eof && s.ends[0].shutdown &&
           s.ends[1].shutdown;
  }

  void dispatch(std::uint64_t token, std::uint32_t events) {
    auto target = tokens_.find(token);
    if (!target) {
      observe("stale-token");
      return;
    }
    auto id = target->session;
    int side = target->side;
    auto& s = *sessions_.at(id);
    try {
      if (draining_ && s.connecting) {
        close(id, "service-stop-connecting", 0);
        return;
      }
      bool connecting_event = s.connecting && side == 1 &&
                              (events & (EPOLLOUT | EPOLLERR | EPOLLHUP));
      if (connecting_event || (events & EPOLLERR)) {
        observe("error-event", &s, side, events);
        int error = socket_error(s.ends[side].fd.get());
        observe("so-error", &s, side, error);
        if (error) throw SessionFailure{"SO_ERROR", error};
        if (connecting_event) {
          s.connecting = false;
          s.deadline.last = Clock::now();
        }
      }
      auto before = s.deadline.last;
      if (!s.connecting && pump(s)) {
        close(id,
              stop_ ? stop_reason_
                    : (draining_ ? "service-stop-drained" : "drained"),
              0);
        return;
      }
      // HUP 不可通过事件掩码屏蔽。无进展时暂挂，定时器主动恢复双向 I/O。
      if ((events & (EPOLLHUP | EPOLLRDHUP)) && s.deadline.last == before) {
        s.ends[side].deferred = Clock::now() + 100ms;
        observe("hup-defer", &s, side);
        remove(s.ends[side]);
      }
      update(s);
    } catch (const SessionFailure& error) {
      statistic(StatKind::Error);
      close(id, error.operation, error.error);
    }
  }

  void deadlines() {
    if (draining_ || stop_) return;
    auto now = Clock::now();
    if (listener_deferred_ && now >= listener_retry_) {
      registration(EPOLL_CTL_ADD, listener_.get(), 1, EPOLLIN);
      listener_deferred_ = false;
    }
    std::vector<std::uint64_t> ids;
    for (auto& [id, session] : sessions_) ids.push_back(id);
    for (auto id : ids) {
      if (draining_ || stop_) return;
      auto& s = *sessions_.at(id);
      if (s.deadline.expired(now, s.connecting ? options_.connect_timeout
                                               : options_.idle_timeout)) {
        statistic(StatKind::Timeout);
        close(id, s.connecting ? "connect-timeout" : "idle-timeout", ETIMEDOUT);
        continue;
      }
      bool retry = false;
      for (auto& end : s.ends) {
        if (end.deferred != Clock::time_point{} && now >= end.deferred) {
          end.deferred = {};
          retry = true;
        }
      }
      if (!retry) continue;
      try {
        observe("hup-retry", &s);
        if (!s.connecting && pump(s)) {
          close(id,
                stop_ ? stop_reason_
                      : (draining_ ? "service-stop-drained" : "drained"),
                0);
          continue;
        }
        update(s);
      } catch (const SessionFailure& error) {
        statistic(StatKind::Error);
        close(id, error.operation, error.error);
      }
    }
  }

  const Callbacks& cb_;
  const Options& options_;
  SignalMask mask_;
  Fd signals_, epoll_, listener_;
  Tokens tokens_;
  std::unordered_map<std::uint64_t, std::unique_ptr<Session>> sessions_;
  std::uint64_t next_id_ = 1;
  Clock::time_point listener_retry_{};
  Clock::time_point drain_deadline_{};
  std::string stop_reason_ = "service-stop-deadline";
  bool listener_deferred_ = false, resource_warning_ = false, stop_ = false,
       draining_ = false;
};
}  // namespace

int run(const Endpoint& listen, const Callbacks& callbacks,
        const Options& options) {
  if (options.drain_timeout.count() <= 0 ||
      options.drain_timeout.count() > std::numeric_limits<int>::max())
    throw std::invalid_argument("invalid TCP drain timeout");
  Reactor reactor(listen, callbacks, options);
  return reactor.loop();
}
}  // namespace l4lb::net
