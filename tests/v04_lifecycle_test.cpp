// Compile the production implementation; friend access observes/sets rare state
// only.
#include <fcntl.h>

#include <deque>
#include <filesystem>
#include <iostream>
#include <random>
#include <thread>

#include "../src/net/reactor.cpp"
#include "../src/net/udp_reactor.cpp"

namespace l4lb::net {
namespace {
void verify(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}

void ring_model() {
  Buffer buffer;
  std::deque<char> reference;
  std::mt19937 random(4096);
  bool wrapped = false;
  for (int operation = 0; operation < 5000; ++operation) {
    if (random() % 3 && buffer.room()) {
      auto count =
          std::min<std::size_t>(1 + random() % 8192, buffer.writable_size());
      for (std::size_t i = 0; i < count; ++i) {
        char byte = static_cast<char>(random());
        buffer.writable()[i] = byte;
        reference.push_back(byte);
      }
      buffer.append(count);
    } else if (buffer.size()) {
      auto count = std::min<std::size_t>(1 + random() % 8192, buffer.size());
      buffer.consume(count);
      while (count--) reference.pop_front();
    }
    verify(buffer.size() == reference.size() &&
               buffer.room() + buffer.size() == kBufferLimit,
           "ring total capacity");
    wrapped |= buffer.readable_size() < buffer.size();
    auto copy = buffer;
    std::size_t at = 0;
    while (copy.size()) {
      auto count = copy.readable_size();
      for (std::size_t i = 0; i < count; ++i)
        verify(copy.data()[i] == reference[at++], "ring reference bytes");
      copy.consume(count);
    }
    bool rejected = false;
    try {
      buffer.append(buffer.writable_size() + 1);
    } catch (const std::logic_error&) {
      rejected = true;
    }
    verify(rejected, "write span bound");
  }
  verify(wrapped, "reference exercises wrap");
  buffer.consume(buffer.size());
  buffer.append(kBufferLimit);
  verify(!buffer.room() && !buffer.writable_size(), "full ring");
  bool rejected = false;
  try {
    buffer.consume(kBufferLimit + 1);
  } catch (const std::logic_error&) {
    rejected = true;
  }
  verify(rejected, "consume bound");
  buffer.consume(kBufferLimit);
  verify(!buffer.size() && buffer.writable_size() == kBufferLimit,
         "empty ring reset");
}

struct ReactorTestAccess {
  static std::array<Fd, 2> add(Reactor& r, std::uint64_t id,
                               bool connecting = false) {
    auto s = std::make_unique<Session>();
    s->id = id;
    s->connecting = connecting;
    std::array<Fd, 2> peers;
    for (int side = 0; side < 2; ++side) {
      int pair[2];
      verify(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                        pair) == 0,
             "socketpair");
      peers[side] = Fd(pair[1]);
      s->ends[side].fd = Fd(pair[0]);
      s->ends[side].token = r.tokens_.add(id, side);
      int small = 4096;
      verify(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &small,
                        sizeof(small)) == 0,
             "send buffer");
    }
    auto& session = *s;
    r.sessions_.emplace(id, std::move(s));
    r.update(session);
    return peers;
  }

  static void exceptions() {
    for (const std::string hook :
         {"session", "statistics", "observe", "diagnostic"}) {
      Options options;
      Callbacks callbacks;
      int notified[3]{};
      Reactor* live = nullptr;
      callbacks.session = [&](const SessionEvent& e) {
        verify(!live->sessions_.contains(e.id),
               "TCP owner invalid before session callback");
        ++notified[0];
        if (hook == "session") throw std::runtime_error("session-primary");
      };
      callbacks.statistics = [&](StatEvent e) {
        if (e.kind == StatKind::Closed) {
          ++notified[1];
          if (hook == "statistics")
            throw std::runtime_error("statistics-primary");
        }
      };
      options.observe = [&](const Observation& e) {
        if (e.kind == "closed") {
          ++notified[2];
          if (hook == "observe") throw std::runtime_error("observe-primary");
        }
      };
      callbacks.diagnostic = [&](const std::string&, int) {
        throw std::runtime_error("diagnostic-primary");
      };
      std::vector<int> descriptors;
      {
        Reactor r({{127, 0, 0, 1}, 0}, callbacks, options);
        live = &r;
        auto peers = add(r, 1);
        for (auto& end : r.sessions_.at(1)->ends)
          descriptors.push_back(end.fd.get());
        if (hook == "diagnostic")
          r.epoll_ = Fd(open("/dev/null", O_RDONLY | O_CLOEXEC));
        std::string message;
        try {
          r.close(1, "explicit", 0);
        } catch (const std::exception& e) {
          message = e.what();
        }
        verify(message == hook + "-primary",
               "TCP first close exception preserved");
        r.close(1, "duplicate", 0);
        verify(r.sessions_.empty() && r.tokens_.size() == 0,
               "TCP tables empty");
        for (int fd : descriptors)
          verify(fcntl(fd, F_GETFD) < 0, "TCP fd released despite callback");
        verify(notified[0] == 1 && notified[1] == 1 && notified[2] == 1,
               "all independent TCP notifications attempted");
      }
      descriptors.clear();
      std::vector<std::array<Fd, 2>> peers;
      std::string original;
      try {
        Reactor r({{127, 0, 0, 1}, 0}, callbacks, options);
        live = &r;
        for (int id = 1; id <= 3; ++id) {
          peers.push_back(add(r, id));
          for (auto& end : r.sessions_.at(id)->ends)
            descriptors.push_back(end.fd.get());
        }
        if (hook == "diagnostic")
          r.epoll_ = Fd(open("/dev/null", O_RDONLY | O_CLOEXEC));
        throw std::runtime_error("original-unwind");
      } catch (const std::exception& e) {
        original = e.what();
      }
      verify(original == "original-unwind", "TCP destructor preserves primary");
      for (int fd : descriptors)
        verify(fcntl(fd, F_GETFD) < 0,
               "TCP all owners released while unwinding");
      verify(notified[0] == 4 && notified[1] == 4 && notified[2] == 4,
             "TCP destructor attempts all notifications");
    }
  }

  static void stop_during_deadlines() {
    Options options;
    options.idle_timeout = 50ms;
    Callbacks callbacks;
    callbacks.ready = [] {};
    int maintenance = 0, timeouts = 0, polls = 0;
    bool requested = false;
    callbacks.maintenance = [&] { ++maintenance; };
    callbacks.statistics = [&](StatEvent e) {
      if (e.kind == StatKind::Timeout) ++timeouts;
    };
    options.observe = [&](const Observation& e) {
      if (e.kind == "poll") ++polls;
      if (e.kind == "hup-retry" && !requested) {
        requested = true;
        kill(getpid(), SIGTERM);
      }
    };
    Reactor r({{127, 0, 0, 1}, 0}, callbacks, options);
    auto peers0 = add(r, 1);
    auto peers1 = add(r, 2);
    auto& first = *r.sessions_.begin()->second;
    first.deadline.last = Clock::now() + 1s;
    first.ends[0].deferred = Clock::now() + 10ms;
    r.registration(EPOLL_CTL_DEL, r.listener_.get(), 1, 0);
    r.listener_deferred_ = true;
    r.listener_retry_ = Clock::now() + 1h;
    r.loop();
    r.listener_retry_ = {};
    r.deadlines();
    verify(r.listener_.get() == -1 && !r.listener_deferred_,
           "deferred listener never revives after stop");
    verify(requested && polls > 0,
           "stop consumed inside post-poll deferred pump");
    verify(
        !maintenance && !timeouts && r.sessions_.empty() && !r.tokens_.size(),
        "transition skips remaining idle work and maintenance");
  }

  static void drain(bool deadline, bool forced, bool negative = false,
                    bool progress = false) {
    Options options;
    options.drain_timeout = 80ms;
    Callbacks callbacks;
    callbacks.ready = [] {};
    int maintenance = 0;
    callbacks.maintenance = [&] { ++maintenance; };
    std::vector<SessionEvent> closed;
    callbacks.session = [&](const SessionEvent& e) { closed.push_back(e); };
    int polls = 0, new_reads = 0;
    bool stopped = false;
    options.observe = [&](const Observation& e) {
      if (e.kind == "poll") ++polls;
      if (stopped && e.kind == "buffer") ++new_reads;
    };
    options.send_call = [&](int fd, const void* data, std::size_t size,
                            int flags) -> ssize_t {
      if (progress && stopped) {
        std::this_thread::sleep_for(1ms);
        size = std::min<std::size_t>(size, 8);
      }
      return send(fd, data, size, flags);
    };
    StopEvent barrier;
    callbacks.stopping = [&](const StopEvent& e) {
      barrier = e;
      stopped = true;
    };
    Reactor r({{127, 0, 0, 1}, 0}, callbacks, options);
    auto peers = add(r, 1);
    auto connecting = add(r, 2, true);
    auto empty = add(r, 3);
    auto& s = *r.sessions_.at(1);
    std::string bytes[2]{std::string(128 * 1024, 'a'),
                         std::string(128 * 1024, 'b')};
    for (int side = 0; side < 2; ++side)
      verify(send(peers[side].get(), bytes[side].data(), bytes[side].size(),
                  MSG_NOSIGNAL) == static_cast<ssize_t>(bytes[side].size()),
             "source sends");
    r.pump(s);
    verify(s.pending[0].size() && s.pending[1].size(),
           "both directions real pending after EAGAIN");
    auto expected0 = s.sent[0] + s.pending[0].size();
    auto expected1 = s.sent[1] + s.pending[1].size();
    auto token = s.ends[0].token;
    kill(getpid(), SIGTERM);
    verify(r.stopping() && stopped && barrier.pending[0] && barrier.pending[1],
           "observed real first signal/pending barrier");
    verify(r.listener_.get() == -1, "listener permanently released");
    auto absolute = r.drain_deadline_;
    auto submitted_before = s.sent[0];
    if (progress) {
      char room[8192];
      verify(recv(peers[1].get(), room, sizeof(room), 0) > 0,
             "make bounded progress opportunity");
    }
    if (negative && deadline) r.drain_deadline_ += 250ms;
    verify(send(peers[0].get(), "after-stop", 10, MSG_NOSIGNAL) == 10,
           "post stop kernel data");
    if (forced) {
      kill(getpid(), SIGINT);
      r.stopping();
      verify(r.drain_deadline_ == absolute &&
                 r.stop_reason_ == "service-stop-forced",
             "second consumed signal does not extend deadline");
    }
    auto start = Clock::now();
    std::string received[2];
    std::exception_ptr background;
    std::thread reader;
    if (!deadline && !forced)
      reader = std::thread([&] {
        try {
          bool eof[2]{};
          auto until = Clock::now() + 1s;
          while ((!eof[0] || !eof[1]) && Clock::now() < until) {
            for (int side = 0; side < 2; ++side) {
              char buf[4096];
              auto n = recv(peers[side].get(), buf, sizeof(buf), 0);
              if (n > 0)
                received[side].append(buf, n);
              else if (!n || (n < 0 && errno == ECONNRESET))
                eof[side] = true;
              else
                verify(errno == EAGAIN || errno == EWOULDBLOCK,
                       "background receive");
            }
            std::this_thread::yield();
          }
        } catch (...) {
          background = std::current_exception();
        }
      });
    std::exception_ptr main_error;
    try {
      r.loop();
    } catch (...) {
      main_error = std::current_exception();
    }
    if (reader.joinable()) reader.join();
    if (main_error) std::rethrow_exception(main_error);
    if (background) std::rethrow_exception(background);
    auto elapsed = Clock::now() - start;
    verify(
        r.sessions_.empty() && !r.tokens_.size() && !new_reads && !maintenance,
        "stopping clears owners without recv/maintenance");
    r.dispatch(token, EPOLLIN | EPOLLOUT);
    verify(polls < 20, "no empty OUT busy loop");
    if (deadline) {
      verify(elapsed >= 60ms && elapsed < 200ms, "bounded absolute deadline");
      if (progress) {
        verify(r.drain_deadline_ == absolute,
               "progress never extends deadline");
        verify(closed.back().sent[0] > submitted_before,
               "real send progress before deadline");
      }
      verify(closed.back().reason == "service-stop-deadline",
             "deadline reason");
    } else if (forced)
      verify(elapsed < 60ms, "second signal immediate");
    else {
      if (negative) bytes[0][0] ^= 1;
      verify(received[1] == bytes[0].substr(0, expected0) &&
                 received[0] == bytes[1].substr(0, expected1),
             "exact pending bytes only both directions");
    }
    std::cout << "drain deadline=" << deadline << " forced=" << forced
              << " pending=" << barrier.pending[0] << ',' << barrier.pending[1]
              << " polls=" << polls << " elapsed_us="
              << std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
                     .count()
              << '\n';
  }
};

struct UdpTestAccess {
  static void add(UdpReactor& r, std::uint64_t id, std::vector<int>& fds) {
    auto flow = std::make_unique<UdpFlow>();
    flow->token = id;
    flow->key.client.port = static_cast<std::uint16_t>(id);
    flow->fd =
        Fd(socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    fds.push_back(flow->fd.get());
    r.keys_.emplace(flow->key, id);
    r.registration(flow->fd.get(), id);
    r.flows_.emplace(id, std::move(flow));
  }

  static void exceptions() {
    for (const std::string hook :
         {"flow", "statistics", "observe", "diagnostic"}) {
      UdpOptions options;
      UdpCallbacks callbacks;
      UdpReactor* live = nullptr;
      int notified[3]{};
      callbacks.flow = [&](const UdpFlowEvent& e) {
        verify(!live->flows_.contains(e.id),
               "UDP owner invalid before flow notification");
        ++notified[0];
        if (hook == "flow") throw std::runtime_error("flow-primary");
      };
      callbacks.statistics = [&](StatEvent e) {
        if (e.kind == StatKind::Closed) {
          ++notified[1];
          if (hook == "statistics")
            throw std::runtime_error("statistics-primary");
        }
      };
      options.observe = [&](const UdpObservation& e) {
        if (e.kind == "test-close" || e.kind == "service-stop") {
          ++notified[2];
          if (hook == "observe") throw std::runtime_error("observe-primary");
        }
      };
      callbacks.diagnostic = [&](const std::string&, int) {
        throw std::runtime_error("diagnostic-primary");
      };
      std::vector<int> fds;
      {
        UdpReactor r({{127, 0, 0, 1}, 0}, callbacks, options);
        live = &r;
        add(r, 3, fds);
        if (hook == "diagnostic")
          r.epoll_ = Fd(open("/dev/null", O_RDONLY | O_CLOEXEC));
        std::string message;
        try {
          r.erase(3, "test-close");
        } catch (const std::exception& e) {
          message = e.what();
        }
        verify(message == hook + "-primary", "UDP first exception");
        r.erase(3, "duplicate");
        verify(r.flows_.empty() && r.keys_.empty(), "UDP identity empty");
        for (int fd : fds) verify(fcntl(fd, F_GETFD) < 0, "UDP fd released");
      }
      fds.clear();
      std::string original;
      try {
        UdpReactor r({{127, 0, 0, 1}, 0}, callbacks, options);
        live = &r;
        for (int id = 3; id < 6; ++id) add(r, id, fds);
        if (hook == "diagnostic")
          r.epoll_ = Fd(open("/dev/null", O_RDONLY | O_CLOEXEC));
        throw std::runtime_error("original-unwind");
      } catch (const std::exception& e) {
        original = e.what();
      }
      verify(original == "original-unwind", "UDP original exception retained");
      for (int fd : fds)
        verify(fcntl(fd, F_GETFD) < 0, "UDP all owners released");
      verify(notified[0] == 4 && notified[1] == 4 && notified[2] == 4,
             "UDP all notifications attempted");
    }
  }
};
}  // namespace
}  // namespace l4lb::net

int main(int argc, char** argv) {
  try {
    using namespace l4lb::net;
    auto fd_count = [] {
      return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                           std::filesystem::directory_iterator{});
    };
    auto before_fd = fd_count();
    sigset_t before_mask{}, after_mask{};
    sigprocmask(SIG_SETMASK, nullptr, &before_mask);
    for (auto bad :
         {0LL, -1LL,
          static_cast<long long>(std::numeric_limits<int>::max()) + 1}) {
      Options invalid;
      invalid.drain_timeout = std::chrono::milliseconds(bad);
      bool rejected = false;
      try {
        run({{127, 0, 0, 1}, 0}, {}, invalid);
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      verify(rejected, "invalid drain deadline rejected before setup");
    }
    ring_model();
    ReactorTestAccess::exceptions();
    ReactorTestAccess::stop_during_deadlines();
    UdpTestAccess::exceptions();
    ReactorTestAccess::drain(false, false,
                             argc > 1 && std::string(argv[1]) == "bytes");
    ReactorTestAccess::drain(true, false,
                             argc > 1 && std::string(argv[1]) == "deadline");
    ReactorTestAccess::drain(true, false, false, true);
    ReactorTestAccess::drain(false, true);
    verify(fd_count() == before_fd, "complete lifecycle fd equality");
    sigprocmask(SIG_SETMASK, nullptr, &after_mask);
    for (int signum : {SIGINT, SIGTERM})
      verify(
          sigismember(&before_mask, signum) == sigismember(&after_mask, signum),
          "signal mask restored");
    std::cout << "fd_before=" << before_fd << " fd_after=" << fd_count()
              << '\n';
    std::cout << "lifecycle PASS: ring model, both-direction pending, "
                 "deadline/second signal, TCP/UDP exception cleanup\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
