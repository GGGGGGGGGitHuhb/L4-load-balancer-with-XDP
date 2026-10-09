#include <fcntl.h>
#include <sys/socket.h>

#include <iostream>
#include <stdexcept>
#include <type_traits>

#include "core/RoundRobinScheduler.h"
#include "net/Fd.h"
#include "net/TcpReactor.h"
#include "net/TcpState.h"
using namespace l4lb;
using namespace l4lb::net;

void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

int main() {
  try {
    bool rejected = false;
    try {
      RoundRobinScheduler empty(0);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "empty pool");
    RoundRobinScheduler rr(3);
    for (int i = 0; i < 100; ++i)
      check(rr.selectNextBackendIndex() == std::size_t(i % 3), "RR order");
    TcpPendingBuffer buffer;
    for (int cycle = 0; cycle < 1000; ++cycle) {
      auto* out = buffer.writable();
      const auto contiguous = buffer.writableBytes();
      for (std::size_t i = 0; i < contiguous; ++i) out[i] = char(i % 251);
      buffer.commitWrittenBytes(contiguous);
      check(buffer.size() == kBufferLimit, "high water");
      buffer.consumeReadableBytes(7);  // 确定性短写消费保留原始尾部。
      check(buffer.data()[0] == char(7), "short write tail");
      const auto left = buffer.size();
      buffer.writable();
      check(buffer.size() == left && buffer.data()[0] == char(7),
            "writable preserves unread tail");
      buffer.consumeReadableBytes(buffer.size());
      check(buffer.room() == kBufferLimit, "reusable capacity");
    }
    rejected = false;
    try {
      buffer.consumeReadableBytes(1);
    } catch (const std::logic_error&) {
      rejected = true;
    }
    check(rejected, "underflow");
    EndpointTokens tokens;
    auto old = tokens.registerEndpoint(1, 0);
    auto peer = tokens.registerEndpoint(1, 1);
    tokens.unregisterEndpoint(old);
    tokens.unregisterEndpoint(peer);
    tokens.unregisterEndpoint(old);
    auto fresh = tokens.registerEndpoint(2, 0);
    check(!tokens.findEndpoint(old) && tokens.findEndpoint(fresh)->sessionId == 2 &&
              tokens.size() == 1,
          "stale batch token");
    tokens.unregisterEndpoint(fresh);
    check(tokens.size() == 0, "tokens empty");
    auto start = Clock::time_point{};
    TcpIdleDeadline deadline{start};
    TcpReactorOptions defaults;
    check(defaults.maxSessions == 1024 &&
              defaults.connectTimeout.count() == 5000 &&
              defaults.idleTimeout.count() == 60000,
          "production constants");
    check(!deadline.expired(start + std::chrono::milliseconds(4999),
                            defaults.connectTimeout),
          "connect before");
    check(deadline.expired(start + std::chrono::seconds(5),
                           defaults.connectTimeout),
          "connect at");
    deadline.recordIoProgress(start + std::chrono::seconds(59), 0);
    check(deadline.expired(start + std::chrono::seconds(60),
                           defaults.idleTimeout),
          "no progress cannot refresh");
    deadline.recordIoProgress(start + std::chrono::seconds(59), 1);
    check(!deadline.expired(start + std::chrono::seconds(60),
                            defaults.idleTimeout),
          "positive refresh");
    check(deadline.expired(start + std::chrono::seconds(119),
                           defaults.idleTimeout),
          "idle at");
    static_assert(!std::is_copy_constructible_v<Fd>);
    int raw = socket(AF_INET, SOCK_STREAM, 0);
    check(raw >= 0, "socket");
    {
      Fd first(raw);
      Fd next(std::move(first));
      check(first.fd() == -1 && next.fd() == raw, "move");
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
