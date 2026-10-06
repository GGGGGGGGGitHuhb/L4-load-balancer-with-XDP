#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>

#include "UdpRuntimeSchema.h"

struct bpf_object;

namespace l4lb::xdp {
/** 提交后的失败必须终止，绝不能回滚或拒绝。 */
class RuntimePublishError final : public std::runtime_error {
 public:
  RuntimePublishError(const std::string& message, bool committed)
      : std::runtime_error(message), committed_(committed) {}

  bool committed() const { return committed_; }

 private:
  bool committed_;
};

/** 借用对象描述符；最多拥有一个 active fd 和一个 candidate fd。 */
class RuntimeMapStore final {
 public:
  explicit RuntimeMapStore(bpf_object* object);
  ~RuntimeMapStore();
  RuntimeMapStore(const RuntimeMapStore&) = delete;
  RuntimeMapStore& operator=(const RuntimeMapStore&) = delete;

  static void validateRuntimeMapObject(bpf_object* object);

  void publishSnapshot(const UdpRuntimeSnapshot& snapshot);

  UdpDsrStatsValue readRuntimeStatistics() const;

  uint64_t generation() const { return generation_; }

 private:
  int outerFd_;

  int statsFd_;

  int activeFd_{-1};

  uint64_t generation_{0};
};
}  // namespace l4lb::xdp
