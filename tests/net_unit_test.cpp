#include <fcntl.h>
#include <sys/socket.h>

#include <iostream>
#include <stdexcept>
#include <type_traits>

#include "core/round_robin.h"
#include "net/fd.h"
#include "net/reactor.h"
#include "net/state.h"
using namespace l4lb;
using namespace l4lb::net;

void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

int main() {
  try {
    bool rejected = false;
    try {
      RoundRobin empty(0);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "empty pool");
    RoundRobin rr(3);
    for (int i = 0; i < 100; ++i)
      check(rr.next() == std::size_t(i % 3), "RR order");
    Buffer buffer;
    for (int cycle = 0; cycle < 1000; ++cycle) {
      auto* out = buffer.writable();
      const auto contiguous = buffer.writable_size();
      for (std::size_t i = 0; i < contiguous; ++i) out[i] = char(i % 251);
      buffer.append(contiguous);
      check(buffer.size() == kBufferLimit, "high water");
      buffer.consume(7);  // 确定性短写消费保留原始尾部。
      check(buffer.data()[0] == char(7), "short write tail");
      const auto left = buffer.size();
      buffer.writable();
      check(buffer.size() == left && buffer.data()[0] == char(7),
            "writable preserves unread tail");
      buffer.consume(buffer.size());
      check(buffer.room() == kBufferLimit, "reusable capacity");
    }
    rejected = false;
    try {
      buffer.consume(1);
    } catch (const std::logic_error&) {
      rejected = true;
    }
    check(rejected, "underflow");
    Tokens tokens;
    auto old = tokens.add(1, 0);
    auto peer = tokens.add(1, 1);
    tokens.erase(old);
    tokens.erase(peer);
    tokens.erase(old);
    auto fresh = tokens.add(2, 0);
    check(!tokens.find(old) && tokens.find(fresh)->session == 2 &&
              tokens.size() == 1,
          "stale batch token");
    tokens.erase(fresh);
    check(tokens.size() == 0, "tokens empty");
    auto start = Clock::time_point{};
    Deadline deadline{start};
    Options defaults;
    check(defaults.max_sessions == 1024 &&
              defaults.connect_timeout.count() == 5000 &&
              defaults.idle_timeout.count() == 60000,
          "production constants");
    check(!deadline.expired(start + std::chrono::milliseconds(4999),
                            defaults.connect_timeout),
          "connect before");
    check(deadline.expired(start + std::chrono::seconds(5),
                           defaults.connect_timeout),
          "connect at");
    deadline.progress(start + std::chrono::seconds(59), 0);
    check(deadline.expired(start + std::chrono::seconds(60),
                           defaults.idle_timeout),
          "no progress cannot refresh");
    deadline.progress(start + std::chrono::seconds(59), 1);
    check(!deadline.expired(start + std::chrono::seconds(60),
                            defaults.idle_timeout),
          "positive refresh");
    check(deadline.expired(start + std::chrono::seconds(119),
                           defaults.idle_timeout),
          "idle at");
    static_assert(!std::is_copy_constructible_v<Fd>);
    int raw = socket(AF_INET, SOCK_STREAM, 0);
    check(raw >= 0, "socket");
    {
      Fd first(raw);
      Fd next(std::move(first));
      check(first.get() == -1 && next.get() == raw, "move");
      next = std::move(next);
    }
    check(fcntl(raw, F_GETFD) == -1, "owner close");
    std::cout << "net unit PASS: RR, buffer/short-tail/compaction, stale "
                 "token, deadlines, fd ownership\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
