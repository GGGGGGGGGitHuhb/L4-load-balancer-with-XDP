#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "MapSchema.h"

struct bpf_object;

namespace l4lb::xdp {
/** 同步配置操作；实现失败时抛出异常。 */
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

/** 借用 map 描述符；寿命不得超过装载它的 bpf_object。 */
class MapStore final : public ConfigMapAccess {
 public:
  explicit MapStore(bpf_object* object);

  static void validateConfigMapObject(bpf_object* object);

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

/** 内核 per-CPU 步长为 value 大小向上对齐到八字节。 */
size_t perCpuStatsBufferSize(int possibleCpus);

uint64_t sumPerCpuPassPackets(std::span<const std::byte> values,
                              int possibleCpus);
}  // namespace l4lb::xdp
