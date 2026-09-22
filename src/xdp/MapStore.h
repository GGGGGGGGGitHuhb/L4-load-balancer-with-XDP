#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "MapSchema.h"

struct bpf_object;

namespace l4lb::xdp {

/** Synchronous configuration operations; implementations throw on failure. */
class ConfigMapAccess {
 public:
  virtual ~ConfigMapAccess() = default;
  virtual void writeBackend(uint32_t index, const XdpBackendValue& value) = 0;
  virtual void writeConfig(const XdpConfigValue& value) = 0;
  virtual XdpBackendValue readBackend(uint32_t index) = 0;
  virtual XdpConfigValue readConfig() = 0;
  virtual void freezeBackends() = 0;
  virtual void freezeConfig() = 0;
};

/** Borrows map descriptors: must not outlive its loaded bpf_object. */
class MapStore final : public ConfigMapAccess {
 public:
  explicit MapStore(bpf_object* object);
  static void validateObject(bpf_object* object);

  void writeBackend(uint32_t index, const XdpBackendValue& value) override;
  void writeConfig(const XdpConfigValue& value) override;
  XdpBackendValue readBackend(uint32_t index) override;
  XdpConfigValue readConfig() override;
  void freezeBackends() override;
  void freezeConfig() override;
  uint64_t readPassPackets() const;

 private:
  int configFd_;
  int backendFd_;
  int statsFd_;
};

/** Kernel per-CPU stride is value size rounded up to eight bytes. */
size_t statsBufferSize(int possibleCpus);
uint64_t sumPassPackets(std::span<const std::byte> values, int possibleCpus);
}  // namespace l4lb::xdp
