#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>

#include "UdpRuntimeSchema.h"

struct bpf_object;

namespace l4lb::xdp {

/** A failure after commit requires termination, never rollback or rejection. */
class RuntimePublishError final : public std::runtime_error {
 public:
  RuntimePublishError(const std::string& message, bool committed)
      : std::runtime_error(message), committed_(committed) {}

  bool committed() const { return committed_; }

 private:
  bool committed_;
};

/** Borrows object descriptors; owns at most one active and one candidate fd. */
class RuntimeMapStore final {
 public:
  explicit RuntimeMapStore(bpf_object* object);
  ~RuntimeMapStore();
  RuntimeMapStore(const RuntimeMapStore&) = delete;
  RuntimeMapStore& operator=(const RuntimeMapStore&) = delete;

  static void validateObject(bpf_object* object);
  void publish(const UdpRuntimeSnapshot& snapshot);
  UdpDsrStatsValue readStats() const;

  uint64_t generation() const { return generation_; }

 private:
  int outerFd_;
  int statsFd_;
  int activeFd_{-1};
  uint64_t generation_{0};
};
}  // namespace l4lb::xdp
