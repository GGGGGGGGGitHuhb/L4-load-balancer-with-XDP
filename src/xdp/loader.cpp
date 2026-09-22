#include "loader.h"

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

uint32_t flags(Mode mode) {
  return mode == Mode::Generic ? XDP_FLAGS_SKB_MODE : XDP_FLAGS_DRV_MODE;
}

[[noreturn]] void fail(const std::string& operation, int error) {
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

uint32_t query(int ifindex, Mode mode) {
  uint32_t id = 0;
  int result = bpf_xdp_query_id(ifindex, flags(mode), &id);
  // Removal of the interface also removes its XDP attachment.
  if (result == -ENODEV) return 0;
  if (result < 0) fail("查询 XDP 程序", -result);
  return id;
}

void detach_fd(int ifindex, Mode mode, int fd, uint32_t id) {
  uint32_t current = query(ifindex, mode);
  if (current == 0) return;
  if (current != id) {
    throw std::runtime_error("程序 ID 不匹配，拒绝卸载其他程序（当前 " +
                             std::to_string(current) + "，预期 " +
                             std::to_string(id) + "）");
  }
  bpf_xdp_attach_opts options{};
  options.sz = sizeof(options);
  // libbpf treats old_prog_fd == 0 as "no comparison". stdin may be closed,
  // so even a valid BPF FD must be moved above the standard descriptor range.
  int comparison_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
  if (comparison_fd < 0) fail("保留条件卸载程序 FD", errno);
  options.old_prog_fd = comparison_fd;
  // Kernel compares the FD atomically: the earlier query is not ownership
  // proof.
  int result = bpf_xdp_detach(ifindex, flags(mode), &options);
  close(comparison_fd);
  if (result == -ENODEV) return;
  if (result < 0) fail("条件卸载 XDP", -result);
}

}  // namespace

const char* mode_name(Mode mode) {
  return mode == Mode::Generic ? "generic" : "native";
}

int interface_index(const std::string& device) {
  unsigned index = if_nametoindex(device.c_str());
  if (index == 0) fail("找不到网络设备 " + device, errno ? errno : ENODEV);
  return static_cast<int>(index);
}

Attachment::~Attachment() {
  if (attached_) {
    try {
      detach();
    } catch (const std::exception& error) {
      std::cerr << "XDP 清理失败：" << error.what() << '\n';
    }
  }
  if (object_) bpf_object__close(object_);
}

void Attachment::load(const std::string& path, bool mapsMode,
                      const std::vector<XdpBackendValue>& backends) {
  if (object_) throw std::runtime_error("不能重复加载对象");

  // Open nonblocking so a FIFO/device path cannot hang the privileged loader.
  struct File {
    int fd;

    ~File() {
      if (fd >= 0) close(fd);
    }
  } file{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)};

  if (file.fd < 0) fail("打开对象 " + path, errno);

  struct stat info {};

  if (fstat(file.fd, &info) < 0) fail("检查对象 " + path, errno);
  if (!S_ISREG(info.st_mode))
    throw std::runtime_error("对象必须是普通文件：" + path);
  constexpr off_t maximum_size = 16 * 1024 * 1024;
  if (info.st_size <= 0 || info.st_size > maximum_size) {
    throw std::runtime_error("对象大小必须大于 0 且不超过 16 MiB：" + path);
  }
  object_bytes_.resize(static_cast<size_t>(info.st_size));
  size_t offset = 0;
  while (offset < object_bytes_.size()) {
    ssize_t count = read(file.fd, object_bytes_.data() + offset,
                         object_bytes_.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) fail("读取对象 " + path, errno);
    if (count == 0) throw std::runtime_error("读取对象时文件被截断：" + path);
    offset += static_cast<size_t>(count);
  }
  // Validate and load the same bounded bytes, not a path that can be replaced.
  object_ =
      bpf_object__open_mem(object_bytes_.data(), object_bytes_.size(), nullptr);
  long error = libbpf_get_error(object_);
  if (error) {
    object_ = nullptr;
    fail("解析 BPF 对象 " + path, static_cast<int>(-error));
  }
  auto* program = bpf_object__next_program(object_, nullptr);
  if (!program || bpf_object__next_program(object_, program) ||
      std::strcmp(bpf_program__name(program),
                  mapsMode ? "xdp_maps_pass" : "xdp_pass") != 0 ||
      std::strcmp(bpf_program__section_name(program), "xdp") != 0 ||
      bpf_program__type(program) != BPF_PROG_TYPE_XDP ||
      (!mapsMode && bpf_object__next_map(object_, nullptr))) {
    throw std::runtime_error(
        mapsMode ? "对象应仅包含 xdp section 的 xdp_maps_pass 程序及规定 maps"
                 : "对象应仅包含 xdp section 的 xdp_pass 程序且无 maps");
  }
  if (mapsMode) MapStore::validateObject(object_);
  int result = bpf_object__load(object_);
  if (result < 0) fail("加载 BPF 对象 " + path, -result);
  program_fd_ = bpf_program__fd(program);
  bpf_prog_info program_info{};
  uint32_t size = sizeof(program_info);
  if (bpf_obj_get_info_by_fd(program_fd_, &program_info, &size) != 0) {
    fail("读取已加载程序 ID", errno);
  }
  program_id_ = program_info.id;
  if (mapsMode) {
    MapStore maps(object_);
    control::synchronizeXdpConfig(maps, backends);
  }
  mapsMode_ = mapsMode;
}

uint64_t Attachment::readPassPackets() const {
  if (!mapsMode_) throw std::runtime_error("未启用 maps 模式");
  return MapStore(object_).readPassPackets();
}

void Attachment::attach(int ifindex, Mode mode) {
  if (program_fd_ < 0 || attached_)
    throw std::runtime_error("无可挂载对象或已经挂载");
  int result = bpf_xdp_attach(
      ifindex, program_fd_, flags(mode) | XDP_FLAGS_UPDATE_IF_NOEXIST, nullptr);
  if (result < 0) fail("挂载 XDP", -result);
  ifindex_ = ifindex;
  mode_ = mode;
  attached_ = true;
}

void Attachment::detach() {
  if (!attached_) return;
  detach_fd(ifindex_, mode_, program_fd_, program_id_);
  attached_ = false;
}

void detach_program(int ifindex, Mode mode, uint32_t expected_id) {
  if (query(ifindex, mode) == 0) return;
  int fd = bpf_prog_get_fd_by_id(expected_id);
  if (fd < 0) fail("打开预期程序 ID " + std::to_string(expected_id), errno);
  try {
    detach_fd(ifindex, mode, fd, expected_id);
  } catch (...) {
    close(fd);
    throw;
  }
  close(fd);
}

}  // namespace l4lb::xdp
