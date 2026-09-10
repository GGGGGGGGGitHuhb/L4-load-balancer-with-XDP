#pragma once
#include <cstddef>
#include <stdexcept>

#include "core/scheduler.h"

namespace l4lb {
/** 固定配置顺序；每次成功调用恰好推进一次，不执行可达性判断。 */
class RoundRobin final : public Scheduler {
 public:
  explicit RoundRobin(std::size_t size) : size_(size) {
    if (size == 0) throw std::invalid_argument("empty backend pool");
  }

  std::size_t next() override {
    const auto selected = next_;
    next_ = next_ == size_ - 1 ? 0 : next_ + 1;
    return selected;
  }

 private:
  std::size_t size_;
  std::size_t next_ = 0;
};
}  // namespace l4lb
