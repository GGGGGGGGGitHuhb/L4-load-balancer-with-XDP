#include "net/reactor.h"

#include <arpa/inet.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
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

  ~Reactor() {
    while (!sessions_.empty())
      close(sessions_.begin()->first, "service-stop", 0);
  }

  int loop() {
    cb_.ready();
    std::array<epoll_event, 128> events{};
    while (!stopping()) {
      deadlines();
      int n = epoll_wait(epoll_.get(), events.data(), events.size(), 100);
      if (n < 0) {
        if (errno == EINTR) continue;
        fail("epoll_wait");
      }
      // 即使高流量始终产生事件，也先处理停止和截止。
      if (stopping()) break;
      deadlines();
      if (cb_.maintenance) cb_.maintenance();
      for (int i = 0; i < n && !stopping(); ++i) {
        auto token = events[i].data.u64;
        if (token == 1) {
          if (events[i].events & (EPOLLERR | EPOLLHUP)) {
            errno = EIO;
            fail("listener epoll");
          }
          if (Clock::now() >= listener_retry_) accept_sessions();
        } else if (token != 2)
          dispatch(token, events[i].events);
      }
      deadlines();
    }
    return 0;
  }

 private:
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
        stop_ = true;
        return true;
      }
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 && errno == EAGAIN) return false;
      if (count < 0) fail("read signalfd");
      errno = EIO;
      fail("read signalfd");
    }
  }

  void registration(int operation, int fd, std::uint64_t token,
                    std::uint32_t mask) {
    epoll_event event{};
    event.data.u64 = token;
    event.events = mask;
    if (epoll_ctl(epoll_.get(), operation, fd, &event) < 0) fail("epoll_ctl");
  }

  void remove(EndpointState& end) {
    if (!end.registered) return;
    if (epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, end.fd.get(), nullptr) < 0 &&
        errno != ENOENT && errno != EBADF)
      cb_.diagnostic("epoll_ctl DEL", errno);
    end.registered = false;
    end.interest = 0;
  }

  void close(std::uint64_t id, const std::string& reason, int error) {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    auto& s = *it->second;
    for (auto& end : s.ends) tokens_.erase(end.token);
    for (auto& end : s.ends) remove(end);
    cb_.session(
        {s.id, s.backend, false, reason, error, {s.sent[0], s.sent[1]}});
    sessions_.erase(it);
    observe("closed");
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
            errno == ENETUNREACH)
          continue;
        if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
            errno == ENOMEM) {
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
        observe("capacity-reject");
        continue;
      }
      auto backend = cb_.select_backend();
      if (!backend) continue;
      auto session = std::make_unique<Session>();
      session->id = next_id_++;
      session->backend = *backend;
      session->ends[0].fd = std::move(front);
      auto id = session->id;
      sessions_.emplace(id, std::move(session));
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
      if (s.connecting) {
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
    while (buffer.size() && budget && !stopping()) {
      auto size = std::min(buffer.size(), budget);
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
      s.deadline.progress(Clock::now(), n);
    }
    if (s.ends[source].paused && buffer.size() <= kLowWater) {
      s.ends[source].paused = false;
      observe("resume", &s, source, buffer.size());
    }
  }

  void read_direction(Session& s, int source) {
    auto& end = s.ends[source];
    auto& buffer = s.pending[source];
    std::size_t budget = kBufferLimit;
    while (!end.eof && !end.paused && budget && !stopping()) {
      auto size = std::min(buffer.room(), budget);
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
        close(id, "drained", 0);
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
      close(id, error.operation, error.error);
    }
  }

  void deadlines() {
    auto now = Clock::now();
    if (listener_deferred_ && now >= listener_retry_) {
      registration(EPOLL_CTL_ADD, listener_.get(), 1, EPOLLIN);
      listener_deferred_ = false;
    }
    std::vector<std::uint64_t> ids;
    for (auto& [id, session] : sessions_) ids.push_back(id);
    for (auto id : ids) {
      auto& s = *sessions_.at(id);
      if (s.deadline.expired(now, s.connecting ? options_.connect_timeout
                                               : options_.idle_timeout)) {
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
          close(id, "drained", 0);
          continue;
        }
        update(s);
      } catch (const SessionFailure& error) {
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
  bool listener_deferred_ = false, resource_warning_ = false, stop_ = false;
};
}  // namespace

int run(const Endpoint& listen, const Callbacks& callbacks,
        const Options& options) {
  Reactor reactor(listen, callbacks, options);
  return reactor.loop();
}
}  // namespace l4lb::net
