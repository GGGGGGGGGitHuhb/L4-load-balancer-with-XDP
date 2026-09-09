#include "net/udp_reactor.h"

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

#include "net/fd.h"

namespace l4lb::net {
namespace {
using UClock = UdpDeadline::Clock;

[[noreturn]] void udp_fail(const char* operation, int error = errno) {
  throw std::system_error(error, std::generic_category(), operation);
}

sockaddr_in udp_address(const Endpoint& e) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(e.port);
  std::memcpy(&a.sin_addr, e.address.data(), 4);
  return a;
}

bool unicast(const std::array<std::uint8_t, 4>& a) {
  return a != std::array<std::uint8_t, 4>{} &&
         a != std::array<std::uint8_t, 4>{255, 255, 255, 255} &&
         !(a[0] >= 224 && a[0] <= 239);
}

bool pressure(int e) {
  return e == EAGAIN || e == EWOULDBLOCK || e == ENOBUFS || e == ENOMEM ||
         e == EMSGSIZE;
}

bool shared_error(int e) {
  return pressure(e) || e == ECONNREFUSED || e == ECONNRESET ||
         e == ENETUNREACH || e == EHOSTUNREACH || e == EACCES;
}

class UdpSignalMask {
 public:
  UdpSignalMask() {
    sigemptyset(&set_);
    sigaddset(&set_, SIGINT);
    sigaddset(&set_, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &set_, &old_) < 0) udp_fail("sigprocmask");
  }

  ~UdpSignalMask() { sigprocmask(SIG_SETMASK, &old_, nullptr); }

  const sigset_t* get() const { return &set_; }

 private:
  sigset_t set_{}, old_{};
};

struct UdpFlow {
  FlowKey key;
  Endpoint backend;
  Fd fd;
  std::uint64_t token;
  UdpDeadline deadline;
};

class UdpReactor {
  friend struct UdpTestAccess;

 public:
  UdpReactor(const Endpoint& endpoint, const UdpCallbacks& cb,
             const UdpOptions& opt)
      : endpoint_(endpoint),
        cb_(cb),
        opt_(opt),
        signals_(signalfd(-1, mask_.get(), SFD_NONBLOCK | SFD_CLOEXEC)),
        epoll_(epoll_create1(EPOLL_CLOEXEC)),
        listener_(
            socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)),
        scratch_(kUdpPayloadLimit) {
    if (signals_.get() < 0) udp_fail("signalfd");
    if (epoll_.get() < 0) udp_fail("epoll_create1");
    if (listener_.get() < 0) udp_fail("UDP listener socket");
    int yes = 1;
    if (setsockopt(listener_.get(), IPPROTO_IP, IP_PKTINFO, &yes, sizeof(yes)) <
        0)
      udp_fail("IP_PKTINFO");
    auto a = udp_address(endpoint);
    if (bind(listener_.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0)
      udp_fail("UDP bind");
    registration(listener_.get(), 1);
    registration(signals_.get(), 2);
  }

  ~UdpReactor() {
    // 析构必须始终释放所有 owner；控制观察函数异常不能中断资源清理。
    while (!flows_.empty()) {
      try {
        erase(flows_.begin()->first, "service-stop");
      } catch (...) {
      }
    }
  }

  int loop() {
    if (cb_.ready) cb_.ready();
    std::array<epoll_event, 128> events{};
    while (!stopping()) {
      expire();
      int count = epoll_wait(epoll_.get(), events.data(), events.size(),
                             static_cast<int>(opt_.poll_interval.count()));
      if (count < 0) {
        if (errno == EINTR) continue;
        udp_fail("UDP epoll_wait");
      }
      if (stopping()) break;
      expire();
      if (cb_.maintenance) cb_.maintenance();
      for (int i = 0; i < count; ++i) {
        if (stopping()) return 0;
        dispatch(events[i].data.u64, events[i].events);
      }
    }
    return 0;
  }

 private:
  void statistic(StatKind kind, std::uint64_t amount = 1) {
    if (cb_.statistics) cb_.statistics({kind, amount});
  }

  UClock::time_point now() const {
    return opt_.now ? opt_.now() : UClock::now();
  }

  void observe(const char* kind, std::uint64_t token = 0, int fd = -1) {
    if (opt_.observe)
      opt_.observe({kind, token, fd, flows_.size(), keys_.size()});
  }

  void diagnostic(const char* kind, int error = 0) {
    observe(kind);
    auto time = now();
    auto it = diagnostic_times_.find(kind);
    if (it != diagnostic_times_.end() &&
        time - it->second < std::chrono::seconds(1))
      return;
    diagnostic_times_[kind] = time;
    if (cb_.diagnostic) cb_.diagnostic(kind, error);
  }

  void registration(int fd, std::uint64_t token) {
    epoll_event e{};
    e.events = EPOLLIN;
    e.data.u64 = token;
    if (epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &e) < 0)
      udp_fail("UDP epoll add");
  }

  bool stopping() {
    signalfd_siginfo info{};
    auto n = read(signals_.get(), &info, sizeof(info));
    if (n == sizeof(info)) return true;
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return false;
    udp_fail("UDP signalfd read", n < 0 ? errno : EIO);
  }

  void erase(std::uint64_t token, const char* reason, int error = 0) {
    auto it = flows_.find(token);
    if (it == flows_.end()) return;
    auto flow = std::move(it->second);
    keys_.erase(flow->key);
    statistic(StatKind::Closed);
    flows_.erase(it);  // 身份先失效，然后 DEL/close；旧批次永远查不到新 owner。
    epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, flow->fd.get(), nullptr);
    int fd = flow->fd.get();
    flow->fd.reset();
    observe(reason, token, fd);
    if (cb_.flow) cb_.flow({token, flow->backend, reason, error});
  }

  void expire() {
    auto time = now();
    for (auto it = flows_.begin(); it != flows_.end();) {
      auto token = it->first;
      bool due = it->second->deadline.expired(time, opt_.idle_timeout);
      ++it;
      if (due) {
        statistic(StatKind::Timeout);
        erase(token, "idle-timeout");
      }
    }
  }

  int setup_error(const char* name, int fd) {
    return opt_.setup_error ? opt_.setup_error(name, fd) : 0;
  }

  std::uint64_t create(const FlowKey& key) {
    expire();
    if (flows_.size() >= opt_.max_flows) {
      statistic(StatKind::Rejected);
      diagnostic("capacity-drop");
      return 0;
    }
    auto selected = cb_.select_backend();  // 真正异常仍传播。
    if (!selected) {
      statistic(StatKind::Rejected);
      return 0;
    }
    Endpoint backend = *selected;
    int error = setup_error("socket", -1);
    Fd fd(error
              ? -1
              : socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (fd.get() < 0) {
      statistic(StatKind::Error);
      diagnostic("setup-drop", error ? error : errno);
      return 0;
    }
    auto a = udp_address(Endpoint{});
    error = setup_error("bind", fd.get());
    if (error ||
        bind(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
      statistic(StatKind::Error);
      diagnostic("setup-drop", error ? error : errno);
      return 0;
    }
    a = udp_address(backend);
    error = setup_error("connect", fd.get());
    if (error ||
        connect(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
      statistic(StatKind::Error);
      diagnostic("setup-drop", error ? error : errno);
      return 0;
    }
    if (next_token_ == std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("UDP token exhausted");
    auto token = next_token_++;
    std::unique_ptr<UdpFlow> flow;
    try {
      flow = std::make_unique<UdpFlow>(
          UdpFlow{key, backend, std::move(fd), token, {now()}});
    } catch (const std::bad_alloc&) {
      statistic(StatKind::Error);
      diagnostic("setup-drop", ENOMEM);
      return 0;
    }
    error = setup_error("epoll", flow->fd.get());
    if (error) {
      statistic(StatKind::Error);
      diagnostic("setup-drop", error);
      return 0;
    }
    // 表分配和 epoll 注册均属于事务；失败不留半索引。
    bool indexed = false;
    try {
      keys_.emplace(key, token);
      indexed = true;
      flows_.emplace(token, std::move(flow));
      registration(flows_.at(token)->fd.get(), token);
    } catch (const std::bad_alloc&) {
      if (indexed) keys_.erase(key);
      flows_.erase(token);
      statistic(StatKind::Error);
      diagnostic("setup-drop", ENOMEM);
      return 0;
    } catch (const std::system_error& e) {
      if (indexed) keys_.erase(key);
      flows_.erase(token);
      if (e.code().value() == EBADF || e.code().value() == EINVAL) throw;
      statistic(StatKind::Error);
      diagnostic("setup-drop", e.code().value());
      return 0;
    }
    statistic(StatKind::Created);
    observe("created", token, flows_.at(token)->fd.get());
    if (cb_.flow) cb_.flow({token, backend, "created"});
    return token;
  }

  bool metadata(const msghdr& msg, const sockaddr_in& from,
                FlowKey& key) const {
    if (msg.msg_namelen < sizeof(sockaddr_in) || from.sin_family != AF_INET ||
        from.sin_port == 0 || (msg.msg_flags & MSG_CTRUNC))
      return false;
    std::memcpy(key.client.address.data(), &from.sin_addr, 4);
    key.client.port = ntohs(from.sin_port);
    if (!unicast(key.client.address)) return false;
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
      std::memcpy(key.local.data(), &info.ipi_addr, 4);
      if (++found != 1 || !unicast(key.local) ||
          info.ipi_addr.s_addr != info.ipi_spec_dst.s_addr)
        return false;
    }
    return found == 1 && (endpoint_.address == std::array<std::uint8_t, 4>{} ||
                          endpoint_.address == key.local);
  }

  ssize_t receive(int fd, msghdr& msg) {
    return opt_.recvmsg_call ? opt_.recvmsg_call(fd, &msg, 0)
                             : recvmsg(fd, &msg, 0);
  }

  bool error(int code, bool listener, std::uint64_t token, const char* kind) {
    if (code == EBADF || code == ENOTSOCK || code == EINVAL)
      udp_fail(kind, code);
    if (listener) {
      if (!shared_error(code)) udp_fail(kind, code);
      if (code != EAGAIN && code != EWOULDBLOCK && code != EINTR)
        statistic(StatKind::Error);
      diagnostic(kind, code);
      return true;
    }
    if (pressure(code)) {
      if (code != EAGAIN && code != EWOULDBLOCK && code != EINTR)
        statistic(StatKind::Error);
      diagnostic(kind, code);
      return true;
    }
    if (code != EAGAIN && code != EWOULDBLOCK && code != EINTR)
      statistic(StatKind::Error);
    erase(token, kind, code);
    return false;
  }

  void send_packet(UdpFlow& flow, std::size_t size, bool reply) {
    iovec vector{scratch_.data(), size};
    msghdr msg{};
    msg.msg_iov = &vector;
    msg.msg_iovlen = 1;
    sockaddr_in to{};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(in_pktinfo))> control{};
    int fd = flow.fd.get();
    if (reply) {
      fd = listener_.get();
      to = udp_address(flow.key.client);
      msg.msg_name = &to;
      msg.msg_namelen = sizeof(to);
      msg.msg_control = control.data();
      msg.msg_controllen = control.size();
      auto* c = CMSG_FIRSTHDR(&msg);
      c->cmsg_level = IPPROTO_IP;
      c->cmsg_type = IP_PKTINFO;
      c->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
      in_pktinfo info{};
      std::memcpy(&info.ipi_spec_dst, flow.key.local.data(), 4);
      std::memcpy(CMSG_DATA(c), &info, sizeof(info));
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
      auto n = opt_.sendmsg_call
                   ? opt_.sendmsg_call(fd, &msg, MSG_NOSIGNAL)
                   : (reply ? sendmsg(fd, &msg, MSG_NOSIGNAL)
                            : send(fd, scratch_.data(), size, MSG_NOSIGNAL));
      if (n >= 0) {
        if (static_cast<std::size_t>(n) == size) {
          statistic(reply ? StatKind::BytesB2c : StatKind::BytesC2b, size);
          statistic(reply ? StatKind::DatagramB2c : StatKind::DatagramC2b);
          flow.deadline.submitted(now());
          observe(reply ? "sent-reply" : "sent-request", flow.token, fd);
        } else {
          statistic(StatKind::Error);
          statistic(StatKind::Dropped);
          diagnostic("short-send-drop");
        }
        return;
      }
      if (errno == EINTR) continue;
      statistic(StatKind::Dropped);
      error(errno, reply, flow.token,
            reply ? "listener-send-drop" : "backend-send-error");
      return;
    }
    statistic(StatKind::Dropped);
    diagnostic("send-budget-drop");
  }

  void listener_read() {
    for (int attempt = 0; attempt < 64; ++attempt) {
      sockaddr_in from{};
      iovec v{scratch_.data(), opt_.receive_size};
      alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(in_pktinfo))>
          control{};
      msghdr msg{};
      msg.msg_name = &from;
      msg.msg_namelen = sizeof(from);
      msg.msg_iov = &v;
      msg.msg_iovlen = 1;
      msg.msg_control = control.data();
      msg.msg_controllen = control.size();
      auto n = receive(listener_.get(), msg);
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        error(errno, true, 0, "listener-recv-error");
        return;
      }
      if ((msg.msg_flags & MSG_TRUNC) ||
          n > static_cast<ssize_t>(kUdpPayloadLimit)) {
        statistic(StatKind::Dropped);
        diagnostic("listener-truncated");
        continue;
      }
      FlowKey key{};
      if (!metadata(msg, from, key)) {
        statistic(StatKind::Dropped);
        diagnostic("metadata-drop");
        continue;
      }
      auto it = keys_.find(key);
      std::uint64_t token = it == keys_.end() ? 0 : it->second;
      if (token &&
          flows_.at(token)->deadline.expired(now(), opt_.idle_timeout)) {
        statistic(StatKind::Timeout);
        erase(token, "idle-timeout");
        token = 0;
      }
      if (!token) {
        try {
          token = create(key);
        } catch (...) {
          // 已取得有效数据报但尚未提交；Error由control统一计，不能在这里重复。
          statistic(StatKind::Dropped);
          throw;
        }
      }
      if (token)
        send_packet(*flows_.at(token), static_cast<std::size_t>(n), false);
      else
        statistic(StatKind::Dropped);
    }
    observe("listener-budget");
  }

  void backend_read(std::uint64_t token) {
    for (int attempt = 0; attempt < 64; ++attempt) {
      auto it = flows_.find(token);
      if (it == flows_.end()) return;
      auto& flow = *it->second;
      if (flow.deadline.expired(now(), opt_.idle_timeout)) {
        statistic(StatKind::Timeout);
        erase(token, "idle-timeout");
        return;
      }
      iovec v{scratch_.data(), opt_.receive_size};
      msghdr msg{};
      msg.msg_iov = &v;
      msg.msg_iovlen = 1;
      auto n = receive(flow.fd.get(), msg);
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        error(errno, false, token, "backend-recv-error");
        return;
      }
      if ((msg.msg_flags & MSG_TRUNC) ||
          n > static_cast<ssize_t>(kUdpPayloadLimit)) {
        statistic(StatKind::Dropped);
        diagnostic("backend-truncated");
        continue;
      }
      send_packet(flow, static_cast<std::size_t>(n), true);
    }
    observe("backend-budget", token);
  }

  void dispatch(std::uint64_t token, std::uint32_t events) {
    if (token == 2) return;
    bool listener = token == 1;
    auto it = flows_.find(token);
    if (!listener && it == flows_.end()) {
      observe("stale-token", token);
      return;
    }
    if (!listener && it->second->deadline.expired(now(), opt_.idle_timeout)) {
      statistic(StatKind::Timeout);
      erase(token, "idle-timeout");
      return;
    }
    int fd = listener ? listener_.get() : it->second->fd.get();
    if (events & EPOLLERR) {
      int code = 0;
      socklen_t len = sizeof(code);
      int rc = opt_.socket_error_call
                   ? opt_.socket_error_call(fd, &code)
                   : getsockopt(fd, SOL_SOCKET, SO_ERROR, &code, &len);
      if (rc < 0) {
        if (listener) udp_fail("listener SO_ERROR");
        if (errno == EBADF || errno == ENOTSOCK) udp_fail("backend SO_ERROR");
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
          statistic(StatKind::Error);
        erase(token, "socket-error-read", errno);
        return;
      }
      observe("epoll-error", token, fd);
      if (code &&
          !error(code, listener, token,
                 listener ? "listener-async-error" : "backend-async-error"))
        return;
    }
    if (events & EPOLLHUP) {
      if (listener) udp_fail("UDP listener HUP", EIO);
      statistic(StatKind::Error);
      erase(token, "backend-hup");
      return;
    }
    if (events & EPOLLIN) {
      if (listener)
        listener_read();
      else
        backend_read(token);
    }
  }

  Endpoint endpoint_;
  const UdpCallbacks& cb_;
  const UdpOptions& opt_;
  UdpSignalMask mask_;
  Fd signals_, epoll_, listener_;
  std::vector<char> scratch_;
  std::uint64_t next_token_ = 3;
  std::unordered_map<FlowKey, std::uint64_t, FlowHash> keys_;
  std::unordered_map<std::uint64_t, std::unique_ptr<UdpFlow>> flows_;
  std::unordered_map<std::string, UClock::time_point> diagnostic_times_;
};
}  // namespace

int run_udp(const Endpoint& endpoint, const UdpCallbacks& callbacks,
            const UdpOptions& options) {
  if (!options.max_flows || options.idle_timeout.count() <= 0 ||
      options.poll_interval.count() <= 0 ||
      options.poll_interval.count() > std::numeric_limits<int>::max() ||
      !options.receive_size || options.receive_size > kUdpPayloadLimit ||
      !callbacks.select_backend)
    throw std::invalid_argument("invalid UDP options/callbacks");
  return UdpReactor(endpoint, callbacks, options).loop();
}
}  // namespace l4lb::net
