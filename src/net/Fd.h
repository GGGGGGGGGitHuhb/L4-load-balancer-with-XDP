#pragma once
#include <unistd.h>

#include <utility>

namespace l4lb::net {
/** 唯一 fd owner。close 不重试 EINTR，避免关闭已复用的描述符。 */
class Fd {
 public:
  explicit Fd(int fd = -1) : fd_(fd) {}

  ~Fd() { closeFd(); }

  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;

  Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      closeFd();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }

  int fd() const { return fd_; }

  void closeFd() {
    if (fd_ >= 0) ::close(std::exchange(fd_, -1));
  }

 private:
  int fd_;
};
}  // namespace l4lb::net
