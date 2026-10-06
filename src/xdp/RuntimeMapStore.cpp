#include "RuntimeMapStore.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <vector>

namespace l4lb::xdp {
namespace {
struct MapSpec {
  const char* mapName;
  bpf_map_type mapType;
  uint32_t valueSize;
  uint32_t mapFlags;
};

constexpr std::array<MapSpec, 3> kMapSpecs{{
    {"l4lb_active_v3", BPF_MAP_TYPE_ARRAY_OF_MAPS, 4, 0},
    {"l4lb_tpl_v3", BPF_MAP_TYPE_ARRAY, sizeof(UdpRuntimeSnapshot),
     BPF_F_RDONLY_PROG},
    {"l4lb_stats_v3", BPF_MAP_TYPE_PERCPU_ARRAY, sizeof(UdpDsrStatsValue), 0},
}};

class CandidateSnapshotFd {
 public:
  explicit CandidateSnapshotFd(int fd) : fd_(fd) {}

  ~CandidateSnapshotFd() {
    if (fd_ >= 0) close(fd_);
  }

  int fd() const { return fd_; }

  int releaseFd() {
    int fd = fd_;
    fd_ = -1;

    return fd;
  }

 private:
  int fd_;
};

[[noreturn]] void throwMapSystemError(const std::string& operation) {
  const int error = errno;
  throw std::runtime_error(operation + " errno=" + std::to_string(error) +
                           ": " + std::strerror(error));
}

bool matchesMapSpecification(bpf_map* map, const MapSpec& spec) {
  return map && bpf_map__type(map) == spec.mapType &&
         bpf_map__key_size(map) == 4 &&
         bpf_map__value_size(map) == spec.valueSize &&
         bpf_map__max_entries(map) == 1 &&
         bpf_map__map_flags(map) == spec.mapFlags;
}

bpf_map_info validateMapMetadata(int fd, const MapSpec& spec, bool checkName) {
  bpf_map_info info{};
  uint32_t size = sizeof(info);
  if (bpf_obj_get_info_by_fd(fd, &info, &size))
    throwMapSystemError("读取 runtime map metadata");

  if ((checkName && std::strcmp(info.name, spec.mapName)) ||
      info.type != static_cast<uint32_t>(spec.mapType) || info.key_size != 4 ||
      info.value_size != spec.valueSize || info.max_entries != 1 ||
      info.map_flags != spec.mapFlags)
    throw std::runtime_error("runtime map metadata 不匹配");

  return info;
}

int validateMapDescriptor(bpf_object* object, const MapSpec& spec) {
  bpf_map* map = bpf_object__find_map_by_name(object, spec.mapName);
  if (!map) throw std::runtime_error("缺少 runtime map");

  int fd = bpf_map__fd(map);
  validateMapMetadata(fd, spec, true);

  return fd;
}

void verifyFrozenSnapshot(int fd, const UdpRuntimeSnapshot& snapshot) {
  uint32_t key = 0;
  errno = 0;
  if (!bpf_map_update_elem(fd, &key, &snapshot, BPF_ANY) ||
      (errno != EPERM && errno != EACCES))
    throw std::runtime_error("runtime inner 未只读冻结");
}

bool isNonzeroUnicastMac(const __u8* mac) {
  return !(mac[0] & 1) && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
}

void validateCandidateSnapshot(const UdpRuntimeSnapshot& snapshot,
                               uint64_t generation) {
  if (snapshot.schemaVersion != 3 || snapshot.backendCount > 64 ||
      snapshot.reserved || generation == std::numeric_limits<uint64_t>::max() ||
      snapshot.generation != generation + 1 || !snapshot.vipAddress ||
      !snapshot.vipPort)
    throw std::invalid_argument("无效 runtime snapshot 或代次");

  const UdpDsrBackendValue empty{};
  for (uint32_t index = 0; index < 64; ++index) {
    const auto& backend = snapshot.backends[index];
    if (index < snapshot.backendCount) {
      if (!backend.ifindex || !isNonzeroUnicastMac(backend.destinationMac) ||
          !isNonzeroUnicastMac(backend.sourceMac))
        throw std::invalid_argument("无效 runtime backend");
    } else if (std::memcmp(&backend, &empty, sizeof(empty))) {
      throw std::invalid_argument("未用 runtime 槽必须清零");
    }
  }
}
}  // namespace

void RuntimeMapStore::validateRuntimeMapObject(bpf_object* object) {
  size_t count = 0;
  bpf_map* map;
  bpf_object__for_each_map(map, object) {
    ++count;
    bool accepted = false;
    for (const auto& spec : kMapSpecs)
      if (!std::strcmp(bpf_map__name(map), spec.mapName))
        accepted = matchesMapSpecification(map, spec);
    if (!accepted) throw std::runtime_error("runtime map 白名单不匹配");
  }

  if (count != kMapSpecs.size())
    throw std::runtime_error("runtime 必须恰好三个 maps");

  for (const auto& spec : kMapSpecs)
    if (!bpf_object__find_map_by_name(object, spec.mapName))
      throw std::runtime_error("缺少 runtime map：" +
                               std::string(spec.mapName));

  auto* outer = bpf_object__find_map_by_name(object, kMapSpecs[0].mapName);
  if (!outer ||
      !matchesMapSpecification(bpf_map__inner_map(outer), kMapSpecs[1]))
    throw std::runtime_error("runtime inner 模板不匹配");

  size_t programs = 0;
  bpf_program* program;
  bpf_object__for_each_program(program, object) {
    ++programs;
    if (std::strcmp(bpf_program__name(program), "xdp_udp_rt") ||
        std::strcmp(bpf_program__section_name(program), "xdp") ||
        bpf_program__type(program) != BPF_PROG_TYPE_XDP)
      throw std::runtime_error("runtime program 白名单不匹配");
  }

  if (programs != 1) throw std::runtime_error("runtime 必须单程序");
}

RuntimeMapStore::RuntimeMapStore(bpf_object* object)
    : outerFd_(validateMapDescriptor(object, kMapSpecs[0])),
      statsFd_(validateMapDescriptor(object, kMapSpecs[2])) {
  // libbpf 在装载后丢弃 inner 模板描述符；validateRuntimeMapObject
  // 必须像程序白名单一样，已在装载前完成检查。
  const int templateFd = validateMapDescriptor(object, kMapSpecs[1]);

  uint32_t key = 0;
  UdpRuntimeSnapshot empty{}, readback{};
  if (bpf_map_update_elem(templateFd, &key, &empty, BPF_ANY))
    throwMapSystemError("初始化 runtime 模板");

  if (bpf_map_lookup_elem(templateFd, &key, &readback))
    throwMapSystemError("回读 runtime 模板");
  if (std::memcmp(&empty, &readback, sizeof(empty)))
    throw std::runtime_error("runtime 模板非零");

  if (bpf_map_freeze(templateFd)) throwMapSystemError("冻结 runtime 模板");
  verifyFrozenSnapshot(templateFd, empty);
}

RuntimeMapStore::~RuntimeMapStore() {
  if (activeFd_ >= 0) close(activeFd_);
}

void RuntimeMapStore::publishSnapshot(const UdpRuntimeSnapshot& snapshot) {
  validateCandidateSnapshot(snapshot, generation_);

  bpf_map_create_opts options{};
  options.sz = sizeof(options);
  options.map_flags = BPF_F_RDONLY_PROG;

  CandidateSnapshotFd candidate(bpf_map_create(
      BPF_MAP_TYPE_ARRAY, "l4lb_snap_v3", 4, sizeof(snapshot), 1, &options));
  if (candidate.fd() < 0) throwMapSystemError("创建 runtime inner");

  uint32_t key = 0;
  if (bpf_map_update_elem(candidate.fd(), &key, &snapshot, BPF_ANY))
    throwMapSystemError("写入 runtime snapshot");

  UdpRuntimeSnapshot readback{};
  if (bpf_map_lookup_elem(candidate.fd(), &key, &readback))
    throwMapSystemError("回读 runtime snapshot");
  if (std::memcmp(&snapshot, &readback, sizeof(snapshot)))
    throw std::runtime_error("runtime snapshot 回读不一致");

  if (bpf_map_freeze(candidate.fd()))
    throwMapSystemError("冻结 runtime snapshot");
  const auto info = validateMapMetadata(candidate.fd(), kMapSpecs[1], false);
  verifyFrozenSnapshot(candidate.fd(), snapshot);

  const int candidateFd = candidate.fd();
  if (bpf_map_update_elem(outerFd_, &key, &candidateFd, BPF_ANY))
    throwMapSystemError("发布 runtime outer");

  // 这是唯一提交点。只回收自己拥有的旧 fd；内核 RCU
  // 保护读取者。
  const int previousFd = activeFd_;
  activeFd_ = candidate.releaseFd();
  generation_ = snapshot.generation;
  if (previousFd >= 0) close(previousFd);

  uint32_t publishedId = 0;
  if (bpf_map_lookup_elem(outerFd_, &key, &publishedId))
    throw RuntimePublishError("已发布后终止：outer 回读失败", true);
  if (publishedId != info.id)
    throw RuntimePublishError("已发布后终止：outer map ID 不一致", true);
}

UdpDsrStatsValue RuntimeMapStore::readRuntimeStatistics() const {
  int cpus = libbpf_num_possible_cpus();
  if (cpus <= 0 ||
      static_cast<size_t>(cpus) >
          std::numeric_limits<size_t>::max() / sizeof(UdpDsrStatsValue))
    throw std::runtime_error("无效 possible CPU 数量");

  std::vector<UdpDsrStatsValue> values(static_cast<size_t>(cpus));

  uint32_t key = 0;
  if (bpf_map_lookup_elem(statsFd_, &key, values.data()))
    throwMapSystemError("读取 runtime stats");

  UdpDsrStatsValue sum{};
  for (const auto& value : values) {
    sum.totalPackets += value.totalPackets;
    sum.passPackets += value.passPackets;
    sum.redirectRequests += value.redirectRequests;
    sum.dropPackets += value.dropPackets;
    sum.unsupportedPackets += value.unsupportedPackets;
    sum.noBackendPackets += value.noBackendPackets;
    sum.invalidConfigPackets += value.invalidConfigPackets;
    sum.helperErrorPackets += value.helperErrorPackets;
  }

  return sum;
}
}  // namespace l4lb::xdp
