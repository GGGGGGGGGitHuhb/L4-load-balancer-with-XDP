// 仅测试目标包含实现以访问局部状态；产品不暴露操纵 flow 的接口。
#include <poll.h>
#include <sys/wait.h>

#include <filesystem>
#include <iostream>
#include <thread>

#include "core/round_robin.h"
#include "net/udp_reactor.cpp"
namespace l4lb::net {
namespace {
using namespace std::chrono_literals;
void require_udp(bool ok, const std::string& why) {
  if (!ok) throw std::runtime_error(why);
}
struct Datagram {
  std::string bytes;
  Endpoint from;
};
Endpoint socket_endpoint(int fd) {
  sockaddr_in a{};
  socklen_t size = sizeof(a);
  require_udp(getsockname(fd, reinterpret_cast<sockaddr*>(&a), &size) == 0,
              "getsockname");
  Endpoint e{};
  e.port = ntohs(a.sin_port);
  std::memcpy(e.address.data(), &a.sin_addr, 4);
  return e;
}
Fd bound_udp(std::array<std::uint8_t, 4> ip = {127, 0, 0, 1}) {
  Fd fd(socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  require_udp(fd.get() >= 0, "test UDP socket");
  auto a = udp_address({ip, 0});
  require_udp(bind(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0,
              "test UDP bind");
  return fd;
}
void put(int fd, Endpoint e, const std::string& bytes) {
  auto a = udp_address(e);
  require_udp(
      sendto(fd, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&a),
             sizeof(a)) == static_cast<ssize_t>(bytes.size()),
      "test send datagram");
}
bool readable(int fd, int ms = 0) {
  pollfd p{fd, POLLIN, 0};
  int n = poll(&p, 1, ms);
  require_udp(n >= 0, "test poll");
  return n && (p.revents & POLLIN);
}
Datagram take(int fd) {
  require_udp(readable(fd, 500),
              "datagram deadline / zero-length EOF mutation");
  std::array<char, 65536> bytes{};
  sockaddr_in from{};
  socklen_t size = sizeof(from);
  auto n = recvfrom(fd, bytes.data(), bytes.size(), 0,
                    reinterpret_cast<sockaddr*>(&from), &size);
  require_udp(n >= 0, "test recv");
  Datagram d;
  d.bytes.assign(bytes.data(), n);
  d.from.port = ntohs(from.sin_port);
  std::memcpy(d.from.address.data(), &from.sin_addr, 4);
  return d;
}
std::size_t fd_count() {
  return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                       std::filesystem::directory_iterator{});
}
struct UdpTestAccess {
  struct Rig {
    Fd a = bound_udp(), b = bound_udp();
    UdpOptions opt;
    UdpCallbacks cb;
    int selects = 0;
    std::vector<UdpObservation> observed;
    std::vector<UdpFlowEvent> lifecycle;
    std::unique_ptr<UdpReactor> r;
    Endpoint listen;
    Rig(bool wildcard = false) {
      cb.select_backend = [&] {
        auto e = socket_endpoint(selects % 2 ? b.get() : a.get());
        ++selects;
        return e;
      };
      cb.flow = [&](const UdpFlowEvent& e) { lifecycle.push_back(e); };
      opt.observe = [&](const UdpObservation& e) { observed.push_back(e); };
      start(wildcard);
    }
    ~Rig() {
      if (std::uncaught_exceptions())
        for (auto& event : observed)
          std::cerr << "observed=" << event.kind << " token=" << event.token
                    << " flows=" << event.flows << '\n';
    }
    void start(bool wildcard = false) {
      r = std::make_unique<UdpReactor>(
          Endpoint{wildcard ? std::array<std::uint8_t, 4>{}
                            : std::array<std::uint8_t, 4>{127, 0, 0, 1},
                   0},
          cb, opt);
      listen = socket_endpoint(r->listener_.get());
      listen.address = {127, 0, 0, 1};
    }
    void pump(int ms = 20) {
      epoll_event events[128]{};
      int n = epoll_wait(r->epoll_.get(), events, 128, ms);
      require_udp(n >= 0, "test epoll_wait");
      r->expire();
      for (int i = 0; i < n; ++i)
        r->dispatch(events[i].data.u64, events[i].events);
    }
    std::uint64_t token() {
      require_udp(r->flows_.size() == 1, "one flow");
      return r->flows_.begin()->first;
    }
    bool seen(const std::string& s) {
      return std::any_of(observed.begin(), observed.end(),
                         [&](auto& e) { return e.kind == s; });
    }
    Datagram request(Fd& c, int backend, const std::string& value,
                     Endpoint dest = {}) {
      if (!dest.port) dest = listen;
      put(c.get(), dest, value);
      pump();
      auto end = UClock::now() + 500ms;
      while (!readable(backend) && UClock::now() < end) pump(5);
      auto d = take(backend);
      require_udp(d.bytes == value, "request payload mismatch");
      return d;
    }
    void reply(Fd& c, int backend, const Datagram& d, const std::string& value,
               Endpoint source = {}) {
      put(backend, d.from, value);
      pump();
      auto end = UClock::now() + 500ms;
      while (!readable(c.get()) && UClock::now() < end) pump(5);
      auto got = take(c.get());
      if (!source.port) source = listen;
      require_udp(got.bytes == value, "reply payload mismatch");
      require_udp(got.from == source, "reply source mismatch");
    }
    void exchange(Fd& c, int backend, const std::string& value,
                  Endpoint dest = {}) {
      auto d = request(c, backend, value, dest);
      reply(c, backend, d, value, dest);
    }
  };
  static void values() {
    FlowKey base{{{127, 0, 0, 1}, 1234}, {127, 0, 0, 1}};
    auto changed = base;
    changed.client.port++;
    require_udp(changed != base, "key port");
    changed = base;
    changed.client.address[3]++;
    require_udp(changed != base, "key client IP");
    changed = base;
    changed.local[3]++;
    require_udp(changed != base, "key destination IP");
    require_udp(FlowHash{}(base) == FlowHash{}(base), "key stable hash");
    UdpDeadline deadline{UClock::time_point{}};
    require_udp(!deadline.expired(UClock::time_point{} + 9ms, 10ms) &&
                    deadline.expired(UClock::time_point{} + 10ms, 10ms),
                "equal timeout");
    deadline.submitted(UClock::time_point{} + 10ms);
    require_udp(!deadline.expired(UClock::time_point{} + 19ms, 10ms),
                "zero success activity");
    UdpCallbacks cb;
    cb.select_backend = [] { return Endpoint{{127, 0, 0, 1}, 1}; };
    for (int which = 0; which < 5; ++which) {
      UdpOptions o;
      if (which == 0) o.max_flows = 0;
      if (which == 1) o.idle_timeout = 0ms;
      if (which == 2) o.poll_interval = 0ms;
      if (which == 3) o.receive_size = 0;
      if (which == 4) o.receive_size = 65508;
      bool rejected = false;
      try {
        run_udp({}, cb, o);
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      require_udp(rejected, "invalid UDP option");
    }
    std::cout << "AC05 UNIT key/deadline/options PASS\n";
  }
  static void identity_and_boundaries() {
    Rig t;
    auto c = bound_udp(), d = bound_udp(), e = bound_udp({127, 0, 0, 2});
    t.exchange(c, t.a.get(), "nonce-A");
    t.exchange(c, t.a.get(), "nonce-A-stable");
    t.exchange(d, t.b.get(), "nonce-B");
    t.exchange(e, t.a.get(), "nonce-A2");
    require_udp(t.selects == 3, "new keys A/B/A, stable selection");
    auto first = t.request(c, t.a.get(), "flow-one");
    auto second = t.request(e, t.a.get(), "flow-two");
    require_udp(first.from != second.from, "distinct backend socket source");
    auto outsider = bound_udp();
    put(outsider.get(), first.from, "forged");
    t.pump();
    require_udp(!readable(c.get()), "foreign backend port leaked");
    t.reply(e, t.a.get(), second, "two-response");
    t.reply(c, t.a.get(), first, "one-response");
    for (std::size_t size : {0, 1, 4096, 65507}) {
      std::string value(size, '\0');
      for (std::size_t i = 0; i < size; ++i)
        value[i] = static_cast<char>(i % 251);
      t.exchange(c, t.a.get(), value);
    }
    auto request = t.request(c, t.a.get(), "multiple");
    for (auto value : {std::string{}, std::string("x"), std::string("a\0b", 3)})
      put(t.a.get(), request.from, value);
    t.pump();
    for (auto value : {std::string{}, std::string("x"), std::string("a\0b", 3)})
      require_udp(take(c.get()).bytes == value, "datagram boundary");
    require_udp(t.selects == 3, "0-length did not end flow");
    std::cout
        << "AC02/04 REAL A/B/A exact nonce bytes, peer filtering, same-backend "
           "interleaving, 0/1/NUL/65507 and packet count PASS\n";
    Rig wild(true);
    auto w = bound_udp();
    auto target = wild.listen;
    target.address = {127, 0, 0, 2};
    wild.exchange(w, wild.a.get(), "local-one");
    wild.exchange(w, wild.b.get(), "local-two", target);
    wild.exchange(w, wild.a.get(), "local-one-again");
    require_udp(wild.selects == 2, "wildcard key");
    std::cout << "AC03 REAL wildcard same client 127.0.0.1/127.0.0.2 separate "
                 "flow and exact source IP PASS\n";
  }
  static void truncation_metadata() {
    Rig t;
    auto c = bound_udp();
    t.opt.receive_size = 4;
    put(c.get(), t.listen, "oversize");
    t.pump();
    require_udp(t.selects == 0 && t.seen("listener-truncated"),
                "real listener MSG_TRUNC");
    auto d = t.request(c, t.a.get(), "abc");
    put(t.a.get(), d.from, "oversize");
    t.pump();
    require_udp(!readable(c.get()) && t.seen("backend-truncated"),
                "real backend MSG_TRUNC whole drop");
    t.reply(c, t.a.get(), d, "ok");
    t.exchange(c, t.a.get(), "");
    require_udp(t.selects == 1, "truncation no reselection");
    std::cout << "AC04 REAL bidirectional MSG_TRUNC small recv buffer no "
                 "prefix; following normal/zero PASS\n";
    for (int mode = 0; mode < 7; ++mode) {
      Rig m;
      auto client = bound_udp();
      m.opt.recvmsg_call = [&](int fd, msghdr* msg, int flags) -> ssize_t {
        auto n = recvmsg(fd, msg, flags);
        if (n < 0) return n;
        if (mode == 0) msg->msg_controllen = 0;
        if (mode == 1) msg->msg_flags |= MSG_CTRUNC;
        auto* cmsg = CMSG_FIRSTHDR(msg);
        if (cmsg && mode >= 2 && mode <= 5) {
          auto* info = reinterpret_cast<in_pktinfo*>(CMSG_DATA(cmsg));
          if (mode == 2) info->ipi_spec_dst.s_addr = 0;
          if (mode == 3)
            info->ipi_addr.s_addr = info->ipi_spec_dst.s_addr =
                htonl(0xe0000001);
          if (mode == 4) cmsg->cmsg_len = CMSG_LEN(sizeof(in_pktinfo)) - 1;
          if (mode == 5)
            info->ipi_addr.s_addr = info->ipi_spec_dst.s_addr =
                htonl(0x7f000002);
        }
        if (mode == 6)
          reinterpret_cast<sockaddr_in*>(msg->msg_name)->sin_port = 0;
        return n;
      };
      put(client.get(), m.listen, "x");
      m.pump();
      require_udp(
          m.selects == 0 && m.r->flows_.empty() && m.seen("metadata-drop"),
          "injected metadata early drop");
    }
    std::cout
        << "AC03 INJECT missing/CTRUNC/specdiff/multicast/incomplete/explicit "
           "mismatch/zero client port before selection PASS\n";
  }
  static void deadlines_capacity_setup() {
    Rig t;
    auto c = bound_udp(), d = bound_udp(), e = bound_udp();
    auto clock = UClock::now();
    t.opt.now = [&] { return clock; };
    t.opt.idle_timeout = 10ms;
    t.opt.max_flows = 2;
    t.exchange(c, t.a.get(), "one");
    t.exchange(d, t.b.get(), "two");
    put(e.get(), t.listen, "third");
    t.pump();
    require_udp(t.selects == 2 && t.r->flows_.size() == 2 &&
                    !readable(t.a.get()) && !readable(t.b.get()),
                "capacity reject no selection");
    clock += 9ms;
    t.exchange(c, t.a.get(), "");  // 零长双向成功刷新。
    clock += 1ms;
    t.r->expire();
    require_udp(t.r->flows_.size() == 1, "equal expiry only idle other flow");
    t.exchange(e, t.a.get(), "third-after-expiry");
    require_udp(t.selects == 3, "capacity release next scheduler");
    clock += 9ms;
    t.r->expire();
    require_udp(t.r->flows_.size() == 1, "zero refresh exact deadline");
    clock += 1ms;
    t.r->expire();
    require_udp(t.r->flows_.empty() && t.r->keys_.empty(),
                "expire all indexes");
    std::cout << "AC05 UNIT/REAL capacity2 no eviction/no selection; exact "
                 "timeout, zero refresh, reuse A PASS\n";
    for (const std::string stage : {"socket", "bind", "connect", "epoll"}) {
      Rig f;
      auto client = bound_udp();
      std::size_t before = fd_count();
      f.opt.setup_error = [&](const char* operation, int) {
        return stage == operation ? (stage == "connect" ? EINPROGRESS : EMFILE)
                                  : 0;
      };
      put(client.get(), f.listen, "failure");
      f.pump();
      require_udp(f.selects == 1 && f.r->flows_.empty() && f.r->keys_.empty() &&
                      fd_count() == before,
                  "setup transaction leak/selection");
      f.opt.setup_error = {};
      f.exchange(client, f.b.get(), "next-B");
      require_udp(f.selects == 2, "failed setup consumed exactly once");
    }
    std::cout << "AC05 INJECT socket/bind/connect(EINPROGRESS)/epoll failure: "
                 "fd and both indexes rollback, next selection B PASS\n";
  }
  static void initial_drop_and_fatal_contracts() {
    auto count = fd_count();
    {
      Rig t;
      auto c = bound_udp();
      auto clock = UClock::now();
      t.opt.now = [&] { return clock; };
      t.opt.idle_timeout = 10ms;
      t.opt.sendmsg_call = [](int, const msghdr*, int) -> ssize_t {
        errno = EAGAIN;
        return -1;
      };
      put(c.get(), t.listen, "first-drop");
      t.pump();
      require_udp(t.selects == 1 && t.r->flows_.size() == 1 &&
                      t.r->flows_.begin()->second->deadline.last == clock,
                  "first EAGAIN bounded lifetime");
      clock += 10ms;
      t.r->expire();
      require_udp(t.r->flows_.empty(), "first drop expiry");
      t.opt.sendmsg_call = {};
      t.r->next_token_ = UINT64_MAX;
      bool exhausted = false;
      put(c.get(), t.listen, "exhaust");
      try {
        t.pump();
      } catch (const std::overflow_error&) {
        exhausted = true;
      }
      require_udp(exhausted && t.r->flows_.empty() && t.r->keys_.empty(),
                  "token exhaustion no wrap");
    }
    {
      Rig t;
      auto c = bound_udp();
      t.cb.select_backend = []() -> Endpoint {
        throw std::runtime_error("selection contract");
      };
      put(c.get(), t.listen, "throw");
      bool thrown = false;
      try {
        t.pump();
      } catch (const std::runtime_error& e) {
        thrown = std::string(e.what()) == "selection contract";
      }
      require_udp(thrown && t.r->flows_.empty(),
                  "selection exception propagates");
    }
    require_udp(fd_count() == count, "fatal contract resource leak");
    std::cout << "AC05/06 INJECT first EAGAIN bounded initial lifetime, token "
                 "exhaustion and selection throw resource cleanup PASS\n";
  }
  static void send_errors() {
    for (bool reply : {false, true}) {
      for (int injected :
           {EAGAIN, EWOULDBLOCK, ENOBUFS, ENOMEM, EMSGSIZE, EINTR, 0}) {
        Rig t;
        auto c = bound_udp();
        auto clock = UClock::now();
        t.opt.now = [&] { return clock; };
        t.opt.idle_timeout = 10ms;
        auto d = t.request(c, t.a.get(), "seed");
        auto token = t.token();
        auto last = t.r->flows_.at(token)->deadline.last;
        clock += 5ms;
        int attempts = 0;
        t.opt.sendmsg_call = [&](int fd, const msghdr* msg,
                                 int flags) -> ssize_t {
          if ((fd == t.r->listener_.get()) == reply) {
            ++attempts;
            if (injected) {
              errno = injected;
              return -1;
            }
            return msg->msg_iov[0].iov_len - 1;
          }
          return sendmsg(fd, msg, flags);
        };
        if (reply) {
          put(t.a.get(), d.from, "drop");
          t.pump();
          require_udp(!readable(c.get()), "reply send drop");
        } else {
          put(c.get(), t.listen, "drop");
          t.pump();
          require_udp(!readable(t.a.get()), "request send drop");
        }
        require_udp(attempts == (injected == EINTR ? 4 : 1) && t.selects == 1 &&
                        t.r->flows_.size() == 1 &&
                        t.r->flows_.at(token)->deadline.last == last,
                    "send classification/no activity/no retry");
        clock += 5ms;
        t.r->expire();
        require_udp(t.r->flows_.empty(), "drop did not refresh timeout");
      }
    }
    Rig t;
    auto c = bound_udp();
    int attempts = 0;
    t.opt.sendmsg_call = [&](int fd, const msghdr* msg, int flags) -> ssize_t {
      if (++attempts <= 3) {
        errno = EINTR;
        return -1;
      }
      return sendmsg(fd, msg, flags);
    };
    t.exchange(c, t.a.get(), "fourth succeeds");
    require_udp(attempts == 5, "EINTR bounded then real success");
    std::cout
        << "AC04/05 INJECT both directions pressure/EMSGSIZE/short/EINTR=4 "
           "drop no queue/no refresh; fourth succeeds PASS\n";
  }
  static void identity_errors() {
    Rig t;
    auto c = bound_udp();
    auto d = t.request(c, t.a.get(), "old");
    auto token = t.token();
    int oldfd = t.r->flows_.at(token)->fd.get();
    put(t.a.get(), d.from, "queued-old");
    int receives = 0;
    t.opt.recvmsg_call = [&](int fd, msghdr* m, int f) {
      ++receives;
      return recvmsg(fd, m, f);
    };
    t.opt.socket_error_call = [](int, int* e) {
      *e = ECONNREFUSED;
      return 0;
    };
    t.r->dispatch(token, EPOLLERR | EPOLLIN);
    require_udp(t.r->flows_.empty() && receives == 0 && !readable(c.get()),
                "ERR|IN read after close");
    t.opt.socket_error_call = {};
    t.opt.recvmsg_call = {};
    auto next = t.request(c, t.b.get(), "new");
    auto fresh = t.token();
    int newfd = t.r->flows_.at(fresh)->fd.get();
    require_udp(newfd == oldfd && fresh != token, "actual fd reuse");
    put(t.b.get(), next.from, "new-reply");
    epoll_event batch[2]{};
    batch[0].data.u64 = token;
    batch[0].events = EPOLLIN;
    batch[1].data.u64 = fresh;
    batch[1].events = EPOLLIN;
    t.r->dispatch(batch[0].data.u64, batch[0].events);
    require_udp(!readable(c.get()), "old token misdelivery");
    t.r->dispatch(batch[1].data.u64, batch[1].events);
    require_udp(take(c.get()).bytes == "new-reply" && t.seen("stale-token"),
                "new token data");
    std::cout << "AC06 REAL fd reuse " << oldfd << " -> " << newfd
              << " INJECT old/new token same batch misdelivery=0; ERR|IN "
                 "close-before-read PASS\n";
    for (int code : {ECONNREFUSED, ECONNRESET, ENETUNREACH, EHOSTUNREACH,
                     EMSGSIZE, EACCES, ENOBUFS, ENOMEM}) {
      t.opt.socket_error_call = [&](int, int* e) {
        *e = code;
        return 0;
      };
      t.r->dispatch(1, EPOLLERR | EPOLLIN);
      require_udp(t.r->flows_.size() == 1, "shared error deleted flow");
    }
    t.opt.socket_error_call = {};
    t.exchange(c, t.b.get(), "shared-still-live");
    t.opt.socket_error_call = [](int, int* e) {
      *e = EBADF;
      return 0;
    };
    bool fatal = false;
    try {
      t.r->dispatch(1, EPOLLERR);
    } catch (const std::system_error&) {
      fatal = true;
    }
    require_udp(fatal, "shared fatal hidden");
    std::cout << "AC06 INJECT shared listener async errors preserve flow and "
                 "subsequent data; EBADF fatal PASS\n";
    Rig icmp;
    auto healthy = bound_udp(), bad = bound_udp();
    icmp.exchange(healthy, icmp.a.get(), "healthy-before");
    Endpoint dead;
    {
      auto vacant = bound_udp();
      dead = socket_endpoint(vacant.get());
    }
    icmp.cb.select_backend = [&] {
      ++icmp.selects;
      return dead;
    };
    put(bad.get(), icmp.listen, "trigger ICMP");
    icmp.pump();
    auto until = UClock::now() + 1s;
    while (icmp.r->flows_.size() > 1 && UClock::now() < until) icmp.pump(20);
    bool refused = false;
    for (auto& event : icmp.lifecycle)
      if (event.error == ECONNREFUSED) refused = true;
    require_udp(refused && icmp.r->flows_.size() == 1,
                "real ICMP/ECONNREFUSED required");
    icmp.exchange(healthy, icmp.a.get(), "healthy-after");
    std::cout << "AC06 REAL connected UDP loopback ICMP ECONNREFUSED one-flow "
                 "cleanup other flow exact bytes PASS\n";
  }
  static void receive_and_shared_errors() {
    for (bool listener : {false, true}) {
      for (int code : {EAGAIN, EMSGSIZE, ECONNREFUSED}) {
        Rig t;
        auto c = bound_udp();
        t.request(c, t.a.get(), "seed");
        auto token = t.token();
        t.opt.recvmsg_call = [&](int, msghdr*, int) -> ssize_t {
          errno = code;
          return -1;
        };
        if (listener)
          t.r->listener_read();
        else
          t.r->backend_read(token);
        require_udp(t.r->flows_.size() == static_cast<std::size_t>(
                                              listener || code != ECONNREFUSED),
                    "recv scoped error classification");
      }
    }
    Rig t;
    auto c = bound_udp();
    auto d = t.request(c, t.a.get(), "seed");
    auto token = t.token();
    t.opt.sendmsg_call = [](int, const msghdr*, int) -> ssize_t {
      errno = ECONNREFUSED;
      return -1;
    };
    put(t.a.get(), d.from, "shared-send");
    t.pump();
    require_udp(t.r->flows_.size() == 1 && !readable(c.get()),
                "shared send doesn't close flow");
    t.opt.sendmsg_call = {};
    t.opt.socket_error_call = [](int, int* e) {
      *e = 0;
      return 0;
    };
    put(t.a.get(), d.from, "zero-error-readable");
    t.r->dispatch(token, EPOLLERR | EPOLLIN);
    require_udp(take(c.get()).bytes == "zero-error-readable",
                "SO_ERROR zero still reads");
    t.opt.socket_error_call = [](int, int*) {
      errno = EIO;
      return -1;
    };
    t.r->dispatch(token, EPOLLERR | EPOLLIN);
    require_udp(t.r->flows_.empty(), "SO_ERROR read failure closes flow");
    t.opt.socket_error_call = {};
    t.request(c, t.b.get(), "new");
    t.r->dispatch(t.token(), EPOLLHUP);
    require_udp(t.r->flows_.empty(), "HUP closes flow");
    auto clock = UClock::now();
    t.opt.now = [&] { return clock; };
    int diagnostics = 0;
    t.cb.diagnostic = [&](const std::string&, int) { ++diagnostics; };
    for (int i = 0; i < 20; ++i) t.r->diagnostic("rate-test", ENOBUFS);
    require_udp(diagnostics == 1, "diagnostic burst rate limit");
    clock += 1s;
    t.r->diagnostic("rate-test", ENOBUFS);
    require_udp(diagnostics == 2, "diagnostic window");
    std::cout
        << "AC04/06 INJECT recv pressure/fatal scoped, shared send refusal "
           "preserved, SO_ERROR zero/failure/HUP and diagnostic1/sec PASS\n";
  }
  static void budgets_cleanup() {
    auto original_count = fd_count();
    sigset_t before{}, after{};
    sigprocmask(SIG_SETMASK, nullptr, &before);
    {
      Rig t;
      auto c = bound_udp();
      int attempts = 0;
      t.opt.recvmsg_call = [&](int, msghdr*, int) -> ssize_t {
        ++attempts;
        errno = EINTR;
        return -1;
      };
      t.r->listener_read();
      require_udp(attempts == 64 && t.seen("listener-budget"),
                  "listener attempt budget");
      t.opt.recvmsg_call = {};
      auto d = t.request(c, t.a.get(), "budget");
      (void)d;
      attempts = 0;
      t.opt.recvmsg_call = [&](int, msghdr*, int) -> ssize_t {
        ++attempts;
        errno = EINTR;
        return -1;
      };
      t.r->backend_read(t.token());
      require_udp(attempts == 64 && t.seen("backend-budget"),
                  "backend attempt budget");
    }
    sigprocmask(SIG_SETMASK, nullptr, &after);
    require_udp(
        fd_count() == original_count &&
            sigismember(&before, SIGINT) == sigismember(&after, SIGINT) &&
            sigismember(&before, SIGTERM) == sigismember(&after, SIGTERM),
        "fd or mask leak");
    {
      Rig t;
      auto c = bound_udp(), idle = bound_udp();
      t.opt.idle_timeout = 30ms;
      t.opt.poll_interval = 5ms;
      t.exchange(idle, t.a.get(), "idle");
      auto parent = getpid();
      auto begin = UClock::now();
      pid_t child = fork();
      require_udp(child >= 0, "load child fork");
      if (child == 0) {
        auto end = UClock::now() + 150ms;
        auto dest = udp_address(t.listen);
        while (UClock::now() < end)
          sendto(c.get(), "hot", 3, 0, reinterpret_cast<sockaddr*>(&dest),
                 sizeof(dest));
        kill(parent, SIGTERM);
        _exit(0);
      }
      int code = t.r->loop();
      int status = 0;
      require_udp(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                      WEXITSTATUS(status) == 0,
                  "load child cleanup");
      require_udp(
          code == 0 && UClock::now() - begin < 2s && t.seen("idle-timeout"),
          "hot traffic stop/expiry bounded");
    }
    sigprocmask(SIG_SETMASK, nullptr, &after);
    require_udp(
        fd_count() == original_count &&
            sigismember(&before, SIGTERM) == sigismember(&after, SIGTERM),
        "hot cleanup mask/fd");
    std::cout << "AC06 REAL hot traffic expiry+SIGTERM bounded, resource/mask "
                 "restoration; INJECT recv EINTR budget64 both fds PASS\n";
  }
  static int run() {
    values();
    identity_and_boundaries();
    truncation_metadata();
    deadlines_capacity_setup();
    initial_drop_and_fatal_contracts();
    send_errors();
    identity_errors();
    receive_and_shared_errors();
    budgets_cleanup();
    return 0;
  }
};
}  // namespace
}  // namespace l4lb::net
int main() {
  try {
    return l4lb::net::UdpTestAccess::run();
  } catch (const std::exception& e) {
    std::cerr << "UDP STATE FAIL " << e.what() << '\n';
    return 1;
  }
}
