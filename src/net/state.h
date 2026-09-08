#pragma once
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
/** 有界连续队列；消费后在下次读入前回收头部空间。 */
class Buffer {
 public:
  std::size_t size() const { return end_ - begin_; }
  std::size_t room() const { return kBufferLimit - size(); }
  const char* data() const { return bytes_.data() + begin_; }
  char* writable() {
    if (begin_) {
      std::memmove(bytes_.data(), data(), size());
      end_ -= begin_;
      begin_ = 0;
    }
    return bytes_.data() + end_;
  }
  void append(std::size_t n) {
    if (n > room()) throw std::logic_error("buffer overflow");
    end_ += n;
  }
  void consume(std::size_t n) {
    if (n > size()) throw std::logic_error("buffer underflow");
    begin_ += n;
    if (begin_ == end_) begin_ = end_ = 0;
  }

 private:
  std::array<char, kBufferLimit> bytes_{};
  std::size_t begin_ = 0, end_ = 0;
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
