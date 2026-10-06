#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "UdpDsrSchema.h"

struct bpf_object;

namespace l4lb::xdp {
/** 同步配置操作；实现失败时抛出异常。 */
class DsrMapAccess {
 public:
  virtual ~DsrMapAccess() = default;

  virtual void writeBackend(uint32_t index,
                            const UdpDsrBackendValue& value) = 0;
  virtual void writeConfig(const UdpDsrConfigValue& value) = 0;

  virtual UdpDsrBackendValue readBackend(uint32_t index) = 0;
  virtual UdpDsrConfigValue readConfig() = 0;

  virtual void freezeBackends() = 0;
  virtual void freezeConfig() = 0;
};

/** 借用 map 描述符；寿命不得超过装载它的 bpf_object。 */
class DsrMapStore final : public DsrMapAccess {
 public:
  explicit DsrMapStore(bpf_object* object);

  static void validateDsrMapObject(bpf_object* object);

  void writeBackend(uint32_t index, const UdpDsrBackendValue& value) override;
  void writeConfig(const UdpDsrConfigValue& value) override;

  UdpDsrBackendValue readBackend(uint32_t index) override;
  UdpDsrConfigValue readConfig() override;

  void freezeBackends() override;
  void freezeConfig() override;

  UdpDsrStatsValue readDsrStatistics() const;

 private:
  int configFd_;
  int backendFd_;

  int statsFd_;
};

}  // namespace l4lb::xdp
