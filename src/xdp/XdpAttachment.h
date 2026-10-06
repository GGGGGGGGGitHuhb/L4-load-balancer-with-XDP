#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "MapSchema.h"
#include "RuntimeMapStore.h"
#include "control/DsrConfigSync.h"

struct bpf_object;

namespace l4lb::xdp {
/** 显式选择挂载模式，绝不静默从 native 回退到 generic。 */
enum class XdpAttachMode { kGeneric, kNative };

/** Mutually exclusive object and configuration ABI. */
enum class XdpObjectProfile { kLegacy, kMapsV1, kUdpDsrV2, kUdpRuntimeV3 };

const char* xdpAttachModeName(XdpAttachMode mode);

int resolveInterfaceIndex(const std::string& device);

/** 拥有已装载对象及其挂载，直到显式清理或析构。 */
class XdpAttachment {
 public:
  XdpAttachment() = default;
  ~XdpAttachment();
  XdpAttachment(const XdpAttachment&) = delete;
  XdpAttachment& operator=(const XdpAttachment&) = delete;

  void loadObject(const std::string& path,
                  XdpObjectProfile profile = XdpObjectProfile::kLegacy,
                  const std::vector<XdpBackendValue>& backends = {},
                  const control::DsrConfiguration& dsr = {},
                  const UdpRuntimeSnapshot* runtime = nullptr);

  void publishRuntime(const UdpRuntimeSnapshot& snapshot);

  UdpDsrStatsValue readRuntimeStats() const;

  UdpDsrStatsValue readDsrStats() const;

  uint64_t readPassPackets() const;

  void attachProgram(int ifindex, XdpAttachMode mode);

  void detachProgram();

  uint32_t programId() const { return programId_; }

 private:
  std::unique_ptr<RuntimeMapStore> runtimeMaps_;

  std::vector<char> objectBytes_;
  bpf_object* object_ = nullptr;

  int programFd_ = -1;
  uint32_t programId_ = 0;

  int interfaceIndex_ = 0;
  XdpAttachMode mode_ = XdpAttachMode::kGeneric;
  bool attached_ = false;
  XdpObjectProfile profile_ = XdpObjectProfile::kLegacy;
};

/** 仅原子卸载请求的程序；若程序已不存在则成功。 */
void detachExpectedProgram(int ifindex, XdpAttachMode mode,
                           uint32_t expectedProgramId);

}  // namespace l4lb::xdp
