#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace l4lb::net {
inline constexpr std::size_t kBufferLimit = 64 * 1024;
inline constexpr std::size_t kLowWater = 32 * 1024;
using Clock = std::chrono::steady_clock;

/** 固定容量环形队列；size/room是总量，I/O只使用连续span。 */
class TcpPendingBuffer {
 public:
  std::size_t size() const { return size_; }

  std::size_t room() const { return kBufferLimit - size_; }

  std::size_t readableBytes() const {
    return std::min(size_, kBufferLimit - readOffset_);
  }

  std::size_t writableBytes() const {
    return std::min(room(), kBufferLimit - writeOffset());
  }

  const char* data() const { return bytes_.data() + readOffset_; }

  char* writable() { return bytes_.data() + writeOffset(); }

  void commitWrittenBytes(std::size_t writtenBytes) {
    if (writtenBytes > writableBytes())
      throw std::logic_error("buffer write span overflow");
    size_ += writtenBytes;
  }

  void consumeReadableBytes(std::size_t consumedBytes) {
    if (consumedBytes > size_) throw std::logic_error("buffer underflow");
    readOffset_ = (readOffset_ + consumedBytes) % kBufferLimit;
    size_ -= consumedBytes;
    if (!size_) readOffset_ = 0;
  }

 private:
  std::size_t writeOffset() const {
    return (readOffset_ + size_) % kBufferLimit;
  }

  std::array<char, kBufferLimit> bytes_{};
  std::size_t readOffset_ = 0, size_ = 0;
};

/** 只以正字节 I/O 刷新 established 空闲计时。 */
struct TcpIdleDeadline {
  Clock::time_point lastIoTime;

  void recordIoProgress(Clock::time_point now, std::size_t bytes) {
    if (bytes) lastIoTime = now;
  }

  bool expired(Clock::time_point now,
               std::chrono::milliseconds duration) const {
    return now - lastIoTime >= duration;
  }
};

/** 单调 token 与裸 fd 解耦，删除后旧批次事件无法命中新 owner。 */
class EndpointTokens {
 public:
  struct Target {
    std::uint64_t session;
    int side;
  };

  std::uint64_t registerEndpoint(std::uint64_t session, int side) {
    if (nextToken_ == UINT64_MAX)
      throw std::overflow_error("endpoint token exhausted");
    const auto token = nextToken_++;
    entries_.emplace(token, Target{session, side});
    return token;
  }

  const Target* findEndpoint(std::uint64_t token) const {
    auto tokenIt = entries_.find(token);
    return tokenIt == entries_.end() ? nullptr : &tokenIt->second;
  }

  void unregisterEndpoint(std::uint64_t token) { entries_.erase(token); }

  std::size_t size() const { return entries_.size(); }

 private:
  std::uint64_t nextToken_ = 3;  // 1=listener, 2=signalfd
  std::unordered_map<std::uint64_t, Target> entries_;
};
}  // namespace l4lb::net
