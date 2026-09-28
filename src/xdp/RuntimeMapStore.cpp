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
  const char* name;
  bpf_map_type type;
  uint32_t valueSize;
  uint32_t flags;
};

constexpr std::array<MapSpec, 3> kMapSpecs{{
    {"l4lb_active_v3", BPF_MAP_TYPE_ARRAY_OF_MAPS, 4, 0},
    {"l4lb_tpl_v3", BPF_MAP_TYPE_ARRAY, sizeof(UdpRuntimeSnapshot),
     BPF_F_RDONLY_PROG},
    {"l4lb_stats_v3", BPF_MAP_TYPE_PERCPU_ARRAY, sizeof(UdpDsrStatsValue), 0},
}};

class CandidateFd {
 public:
  explicit CandidateFd(int fd) : fd_(fd) {}

  ~CandidateFd() {
    if (fd_ >= 0) close(fd_);
  }

  int get() const { return fd_; }

  int release() {
    int fd = fd_;
    fd_ = -1;
    return fd;
  }

 private:
  int fd_;
};

[[noreturn]] void mapFailure(const std::string& operation) {
  const int error = errno;
  throw std::runtime_error(operation + " errno=" + std::to_string(error) +
                           ": " + std::strerror(error));
}

bool matches(bpf_map* map, const MapSpec& spec) {
  return map && bpf_map__type(map) == spec.type &&
         bpf_map__key_size(map) == 4 &&
         bpf_map__value_size(map) == spec.valueSize &&
         bpf_map__max_entries(map) == 1 &&
         bpf_map__map_flags(map) == spec.flags;
}

bpf_map_info validatedInfo(int fd, const MapSpec& spec, bool checkName) {
  bpf_map_info info{};
  uint32_t size = sizeof(info);
  if (bpf_obj_get_info_by_fd(fd, &info, &size))
    mapFailure("读取 runtime map metadata");
  if ((checkName && std::strcmp(info.name, spec.name)) ||
      info.type != static_cast<uint32_t>(spec.type) || info.key_size != 4 ||
      info.value_size != spec.valueSize || info.max_entries != 1 ||
      info.map_flags != spec.flags)
    throw std::runtime_error("runtime map metadata 不匹配");
  return info;
}

int validatedFd(bpf_object* object, const MapSpec& spec) {
  bpf_map* map = bpf_object__find_map_by_name(object, spec.name);
  if (!map) throw std::runtime_error("缺少 runtime map");
  int fd = bpf_map__fd(map);
  validatedInfo(fd, spec, true);
  return fd;
}

void verifyFrozen(int fd, const UdpRuntimeSnapshot& snapshot) {
  uint32_t key = 0;
  errno = 0;
  if (!bpf_map_update_elem(fd, &key, &snapshot, BPF_ANY) ||
      (errno != EPERM && errno != EACCES))
    throw std::runtime_error("runtime inner 未只读冻结");
}

bool validMac(const __u8* mac) {
  return !(mac[0] & 1) && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
}

void validateSnapshot(const UdpRuntimeSnapshot& snapshot, uint64_t generation) {
  if (snapshot.schemaVersion != 3 || snapshot.backendCount > 64 ||
      snapshot.reserved || generation == std::numeric_limits<uint64_t>::max() ||
      snapshot.generation != generation + 1 || !snapshot.vipAddress ||
      !snapshot.vipPort)
    throw std::invalid_argument("无效 runtime snapshot 或代次");
  const UdpDsrBackendValue empty{};
  for (uint32_t index = 0; index < 64; ++index) {
    const auto& backend = snapshot.backends[index];
    if (index < snapshot.backendCount) {
      if (!backend.ifindex || !validMac(backend.destinationMac) ||
          !validMac(backend.sourceMac))
        throw std::invalid_argument("无效 runtime backend");
    } else if (std::memcmp(&backend, &empty, sizeof(empty))) {
      throw std::invalid_argument("未用 runtime 槽必须清零");
    }
  }
}
}  // namespace

void RuntimeMapStore::validateObject(bpf_object* object) {
  size_t count = 0;
  bpf_map* map;
  bpf_object__for_each_map(map, object) {
    ++count;
    bool accepted = false;
    for (const auto& spec : kMapSpecs)
      if (!std::strcmp(bpf_map__name(map), spec.name))
        accepted = matches(map, spec);
    if (!accepted) throw std::runtime_error("runtime map 白名单不匹配");
  }
  if (count != kMapSpecs.size())
    throw std::runtime_error("runtime 必须恰好三个 maps");
  for (const auto& spec : kMapSpecs)
    if (!bpf_object__find_map_by_name(object, spec.name))
      throw std::runtime_error("缺少 runtime map：" + std::string(spec.name));
  auto* outer = bpf_object__find_map_by_name(object, kMapSpecs[0].name);
  if (!outer || !matches(bpf_map__inner_map(outer), kMapSpecs[1]))
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
    : outerFd_(validatedFd(object, kMapSpecs[0])),
      statsFd_(validatedFd(object, kMapSpecs[2])) {
  // libbpf discards the inner template descriptor after load; validateObject
  // must already have checked it before load, just like the program allowlist.
  const int templateFd = validatedFd(object, kMapSpecs[1]);
  uint32_t key = 0;
  UdpRuntimeSnapshot empty{}, readback{};
  if (bpf_map_update_elem(templateFd, &key, &empty, BPF_ANY))
    mapFailure("初始化 runtime 模板");
  if (bpf_map_lookup_elem(templateFd, &key, &readback))
    mapFailure("回读 runtime 模板");
  if (std::memcmp(&empty, &readback, sizeof(empty)))
    throw std::runtime_error("runtime 模板非零");
  if (bpf_map_freeze(templateFd)) mapFailure("冻结 runtime 模板");
  verifyFrozen(templateFd, empty);
}

RuntimeMapStore::~RuntimeMapStore() {
  if (activeFd_ >= 0) close(activeFd_);
}

void RuntimeMapStore::publish(const UdpRuntimeSnapshot& snapshot) {
  validateSnapshot(snapshot, generation_);
  bpf_map_create_opts options{};
  options.sz = sizeof(options);
  options.map_flags = BPF_F_RDONLY_PROG;
  CandidateFd candidate(bpf_map_create(BPF_MAP_TYPE_ARRAY, "l4lb_snap_v3", 4,
                                       sizeof(snapshot), 1, &options));
  if (candidate.get() < 0) mapFailure("创建 runtime inner");
  uint32_t key = 0;
  if (bpf_map_update_elem(candidate.get(), &key, &snapshot, BPF_ANY))
    mapFailure("写入 runtime snapshot");
  UdpRuntimeSnapshot readback{};
  if (bpf_map_lookup_elem(candidate.get(), &key, &readback))
    mapFailure("回读 runtime snapshot");
  if (std::memcmp(&snapshot, &readback, sizeof(snapshot)))
    throw std::runtime_error("runtime snapshot 回读不一致");
  if (bpf_map_freeze(candidate.get())) mapFailure("冻结 runtime snapshot");
  const auto info = validatedInfo(candidate.get(), kMapSpecs[1], false);
  verifyFrozen(candidate.get(), snapshot);
  const int candidateFd = candidate.get();
  if (bpf_map_update_elem(outerFd_, &key, &candidateFd, BPF_ANY))
    mapFailure("发布 runtime outer");

  // This is the unique commit point. Retire only our old fd; kernel RCU
  // protects readers.
  const int previousFd = activeFd_;
  activeFd_ = candidate.release();
  generation_ = snapshot.generation;
  if (previousFd >= 0) close(previousFd);
  uint32_t publishedId = 0;
  if (bpf_map_lookup_elem(outerFd_, &key, &publishedId))
    throw RuntimePublishError("已发布后终止：outer 回读失败", true);
  if (publishedId != info.id)
    throw RuntimePublishError("已发布后终止：outer map ID 不一致", true);
}

UdpDsrStatsValue RuntimeMapStore::readStats() const {
  int cpus = libbpf_num_possible_cpus();
  if (cpus <= 0 ||
      static_cast<size_t>(cpus) >
          std::numeric_limits<size_t>::max() / sizeof(UdpDsrStatsValue))
    throw std::runtime_error("无效 possible CPU 数量");
  std::vector<UdpDsrStatsValue> values(static_cast<size_t>(cpus));
  uint32_t key = 0;
  if (bpf_map_lookup_elem(statsFd_, &key, values.data()))
    mapFailure("读取 runtime stats");
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
