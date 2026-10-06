#include "XdpAttachment.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <fcntl.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "MapStore.h"
#include "control/XdpConfigSync.h"

namespace l4lb::xdp {
namespace {
uint32_t xdpAttachFlags(XdpAttachMode mode) {
  return mode == XdpAttachMode::kGeneric ? XDP_FLAGS_SKB_MODE
                                         : XDP_FLAGS_DRV_MODE;
}

[[noreturn]] void throwXdpSystemError(const std::string& operation, int error) {
  std::string hint;
  if (error == EPERM || error == EACCES) {
    hint = "；请检查 sudo、CAP_BPF/CAP_NET_ADMIN、容器限制及 memlock 限额";
  } else if (error == EOPNOTSUPP || error == EINVAL) {
    hint =
        "；请检查内核/网卡 XDP 支持及对象；native 不支持时可显式选择 --mode "
        "generic";
  } else if (error == EBUSY || error == EEXIST) {
    hint = "；设备可能已有程序或程序已被替换，拒绝覆盖/卸载其他程序";
  } else if (error == ENOMEM) {
    hint = "；请检查可用内存及 memlock 限额";
  }

  throw std::runtime_error(operation + ": " + std::strerror(error) + hint);
}

uint32_t queryAttachedProgramId(int ifindex, XdpAttachMode mode) {
  uint32_t id = 0;
  int result = bpf_xdp_query_id(ifindex, xdpAttachFlags(mode), &id);

  // 接口被移除时，其 XDP 挂载也随之移除。
  if (result == -ENODEV) return 0;
  if (result < 0) throwXdpSystemError("查询 XDP 程序", -result);

  return id;
}

void detachExpectedProgramFd(int ifindex, XdpAttachMode mode, int fd,
                             uint32_t id) {
  uint32_t current = queryAttachedProgramId(ifindex, mode);
  if (current == 0) return;

  if (current != id) {
    throw std::runtime_error("程序 ID 不匹配，拒绝卸载其他程序（当前 " +
                             std::to_string(current) + "，预期 " +
                             std::to_string(id) + "）");
  }

  bpf_xdp_attach_opts options{};
  options.sz = sizeof(options);

  // libbpf 将 old_prog_fd == 0 视为“不比较”。stdin 可能已关闭，
  // 因此即使是有效 BPF fd，也必须移到标准描述符范围之外。
  int comparisonFd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
  if (comparisonFd < 0) throwXdpSystemError("保留条件卸载程序 FD", errno);
  options.old_prog_fd = comparisonFd;

  // 内核原子比较 fd；先前的查询不能证明
  // 所有权。
  int result = bpf_xdp_detach(ifindex, xdpAttachFlags(mode), &options);
  close(comparisonFd);

  if (result == -ENODEV) return;
  if (result < 0) throwXdpSystemError("条件卸载 XDP", -result);
}

}  // namespace

const char* xdpAttachModeName(XdpAttachMode mode) {
  return mode == XdpAttachMode::kGeneric ? "generic" : "native";
}

int resolveInterfaceIndex(const std::string& device) {
  unsigned index = if_nametoindex(device.c_str());
  if (index == 0)
    throwXdpSystemError("找不到网络设备 " + device, errno ? errno : ENODEV);

  return static_cast<int>(index);
}

XdpAttachment::~XdpAttachment() {
  if (attached_) {
    try {
      detachProgram();
    } catch (const std::exception& error) {
      std::cerr << "XDP 清理失败：" << error.what() << '\n';
    }
  }

  if (object_) bpf_object__close(object_);
}

void XdpAttachment::loadObject(const std::string& path,
                               XdpObjectProfile profile,
                               const std::vector<XdpBackendValue>& backends,
                               const control::DsrConfiguration& dsr,
                               const UdpRuntimeSnapshot* runtime) {
  if (object_) throw std::runtime_error("不能重复加载对象");

  // 以非阻塞方式打开，避免 FIFO 或设备路径挂住特权 loader。
  struct File {
    int fd;

    ~File() {
      if (fd >= 0) close(fd);
    }
  } file{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)};

  if (file.fd < 0) throwXdpSystemError("打开对象 " + path, errno);

  struct stat info {};

  if (fstat(file.fd, &info) < 0) throwXdpSystemError("检查对象 " + path, errno);
  if (!S_ISREG(info.st_mode))
    throw std::runtime_error("对象必须是普通文件：" + path);

  constexpr off_t kMaxObjectBytes = 16 * 1024 * 1024;
  if (info.st_size <= 0 || info.st_size > kMaxObjectBytes) {
    throw std::runtime_error("对象大小必须大于 0 且不超过 16 MiB：" + path);
  }

  objectBytes_.resize(static_cast<size_t>(info.st_size));
  size_t offset = 0;
  while (offset < objectBytes_.size()) {
    ssize_t count = read(file.fd, objectBytes_.data() + offset,
                         objectBytes_.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) throwXdpSystemError("读取对象 " + path, errno);
    if (count == 0) throw std::runtime_error("读取对象时文件被截断：" + path);
    offset += static_cast<size_t>(count);
  }

  // 校验和装载同一份有大小上限的字节，而非可被替换的路径。
  object_ =
      bpf_object__open_mem(objectBytes_.data(), objectBytes_.size(), nullptr);
  long error = libbpf_get_error(object_);
  if (error) {
    object_ = nullptr;
    throwXdpSystemError("解析 BPF 对象 " + path, static_cast<int>(-error));
  }

  const bool mapsMode = profile == XdpObjectProfile::kMapsV1;
  const bool dsrMode = profile == XdpObjectProfile::kUdpDsrV2;
  const bool runtimeMode = profile == XdpObjectProfile::kUdpRuntimeV3;
  const char* programName = runtimeMode ? "xdp_udp_rt"
                            : dsrMode   ? "xdp_udp_dsr"
                            : mapsMode  ? "xdp_maps_pass"
                                        : "xdp_pass";

  auto* program = bpf_object__next_program(object_, nullptr);
  if (!program || bpf_object__next_program(object_, program) ||
      std::strcmp(bpf_program__name(program), programName) != 0 ||
      std::strcmp(bpf_program__section_name(program), "xdp") != 0 ||
      bpf_program__type(program) != BPF_PROG_TYPE_XDP ||
      (!mapsMode && !dsrMode && !runtimeMode &&
       bpf_object__next_map(object_, nullptr))) {
    throw std::runtime_error(
        runtimeMode ? "对象应仅包含 xdp_udp_rt 及规定 v3 maps"
        : dsrMode ? "对象应仅包含 xdp section 的 xdp_udp_dsr 程序及规定 maps"
        : mapsMode ? "对象应仅包含 xdp section 的 xdp_maps_pass 程序及规定 maps"
                   : "对象应仅包含 xdp section 的 xdp_pass 程序且无 maps");
  }

  if (runtimeMode) RuntimeMapStore::validateRuntimeMapObject(object_);
  if (mapsMode) MapStore::validateConfigMapObject(object_);
  if (dsrMode) DsrMapStore::validateDsrMapObject(object_);

  int result = bpf_object__load(object_);
  if (result < 0) throwXdpSystemError("加载 BPF 对象 " + path, -result);

  programFd_ = bpf_program__fd(program);
  bpf_prog_info programInfo{};
  uint32_t size = sizeof(programInfo);
  if (bpf_obj_get_info_by_fd(programFd_, &programInfo, &size) != 0) {
    throwXdpSystemError("读取已加载程序 ID", errno);
  }

  programId_ = programInfo.id;

  if (mapsMode) {
    MapStore maps(object_);
    control::synchronizeXdpConfig(maps, backends);
  }
  if (dsrMode) {
    DsrMapStore maps(object_);
    control::synchronizeDsrConfig(maps, dsr);
  }
  if (runtimeMode) {
    if (!runtime) throw std::invalid_argument("缺少 runtime 初始快照");
    runtimeMaps_ = std::make_unique<RuntimeMapStore>(object_);
    runtimeMaps_->publishSnapshot(*runtime);
  }

  profile_ = profile;
}

void XdpAttachment::publishRuntime(const UdpRuntimeSnapshot& snapshot) {
  if (!runtimeMaps_) throw std::logic_error("runtime profile 未加载");
  runtimeMaps_->publishSnapshot(snapshot);
}

UdpDsrStatsValue XdpAttachment::readRuntimeStats() const {
  if (!runtimeMaps_) throw std::logic_error("runtime profile 未加载");
  return runtimeMaps_->readRuntimeStatistics();
}

uint64_t XdpAttachment::readPassPackets() const {
  if (profile_ != XdpObjectProfile::kMapsV1)
    throw std::runtime_error("未启用 maps 模式");
  return MapStore(object_).readPassPackets();
}

UdpDsrStatsValue XdpAttachment::readDsrStats() const {
  if (profile_ != XdpObjectProfile::kUdpDsrV2)
    throw std::runtime_error("未启用 udp-dsr 模式");
  return DsrMapStore(object_).readDsrStatistics();
}

void XdpAttachment::attachProgram(int ifindex, XdpAttachMode mode) {
  if (programFd_ < 0 || attached_)
    throw std::runtime_error("无可挂载对象或已经挂载");

  int result = bpf_xdp_attach(
      ifindex, programFd_, xdpAttachFlags(mode) | XDP_FLAGS_UPDATE_IF_NOEXIST,
      nullptr);
  if (result < 0) throwXdpSystemError("挂载 XDP", -result);

  interfaceIndex_ = ifindex;
  mode_ = mode;
  attached_ = true;
}

void XdpAttachment::detachProgram() {
  if (!attached_) return;
  detachExpectedProgramFd(interfaceIndex_, mode_, programFd_, programId_);

  attached_ = false;
}

void detachExpectedProgram(int ifindex, XdpAttachMode mode,
                           uint32_t expectedProgramId) {
  if (queryAttachedProgramId(ifindex, mode) == 0) return;

  int fd = bpf_prog_get_fd_by_id(expectedProgramId);
  if (fd < 0)
    throwXdpSystemError("打开预期程序 ID " + std::to_string(expectedProgramId),
                        errno);

  try {
    detachExpectedProgramFd(ifindex, mode, fd, expectedProgramId);
  } catch (...) {
    close(fd);
    throw;
  }

  close(fd);
}

}  // namespace l4lb::xdp
