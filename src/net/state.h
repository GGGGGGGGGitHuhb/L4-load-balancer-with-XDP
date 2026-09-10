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
class Buffer {
 public:
  std::size_t size() const { return size_; }

  std::size_t room() const { return kBufferLimit - size_; }

  std::size_t readable_size() const {
    return std::min(size_, kBufferLimit - begin_);
  }

  std::size_t writable_size() const {
    return std::min(room(), kBufferLimit - tail());
  }

  const char* data() const { return bytes_.data() + begin_; }

  char* writable() { return bytes_.data() + tail(); }

  void append(std::size_t n) {
    if (n > writable_size())
      throw std::logic_error("buffer write span overflow");
    size_ += n;
  }

  void consume(std::size_t n) {
    if (n > size_) throw std::logic_error("buffer underflow");
    begin_ = (begin_ + n) % kBufferLimit;
    size_ -= n;
    if (!size_) begin_ = 0;
  }

 private:
  std::size_t tail() const { return (begin_ + size_) % kBufferLimit; }

  std::array<char, kBufferLimit> bytes_{};
  std::size_t begin_ = 0, size_ = 0;
};

/** 只以正字节 I/O 刷新 established 空闲计时。 */
struct Deadline {
  Clock::time_point last;

  void progress(Clock::time_point now, std::size_t bytes) {
    if (bytes) last = now;
  }

  bool expired(Clock::time_point now,
               std::chrono::milliseconds duration) const {
    return now - last >= duration;
  }
};

/** 单调 token 与裸 fd 解耦，删除后旧批次事件无法命中新 owner。 */
class Tokens {
 public:
  struct Target {
    std::uint64_t session;
    int side;
  };

  std::uint64_t add(std::uint64_t session, int side) {
    if (next_ == UINT64_MAX)
      throw std::overflow_error("endpoint token exhausted");
    const auto token = next_++;
    entries_.emplace(token, Target{session, side});
    return token;
  }

  const Target* find(std::uint64_t token) const {
    auto it = entries_.find(token);
    return it == entries_.end() ? nullptr : &it->second;
  }

  void erase(std::uint64_t token) { entries_.erase(token); }

  std::size_t size() const { return entries_.size(); }

 private:
  std::uint64_t next_ = 3;  // 1=listener, 2=signalfd
  std::unordered_map<std::uint64_t, Target> entries_;
};
}  // namespace l4lb::net
