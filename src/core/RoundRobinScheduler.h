#pragma once
#include <cstddef>
#include <stdexcept>

#include "core/BackendScheduler.h"

namespace l4lb {
/** 固定配置顺序；每次成功调用恰好推进一次，不执行可达性判断。 */
class RoundRobinScheduler final : public BackendScheduler {
 public:
  explicit RoundRobinScheduler(std::size_t backendCount)
      : backendCount_(backendCount) {
    if (backendCount == 0) throw std::invalid_argument("empty backend pool");
  }

  std::size_t selectNextBackendIndex() override {
    const auto selected = nextBackendIndex_;
    nextBackendIndex_ =
        nextBackendIndex_ == backendCount_ - 1 ? 0 : nextBackendIndex_ + 1;
    return selected;
  }

 private:
  std::size_t backendCount_;
  std::size_t nextBackendIndex_ = 0;
};
}  // namespace l4lb
