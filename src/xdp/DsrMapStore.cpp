#include "DsrMapStore.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace l4lb::xdp {
namespace {
struct MapSpec {
  const char* mapName;
  bpf_map_type mapType;
  uint32_t valueSize;
  uint32_t maxEntries;
  uint32_t mapFlags;
};

constexpr std::array<MapSpec, 3> kMapSpecs{
    {{"l4lb_cfg_v2", BPF_MAP_TYPE_ARRAY, sizeof(UdpDsrConfigValue), 1,
      BPF_F_RDONLY_PROG},
     {"l4lb_be_v2", BPF_MAP_TYPE_ARRAY, sizeof(UdpDsrBackendValue),
      L4LB_DSR_MAX_BACKENDS, BPF_F_RDONLY_PROG},
     {"l4lb_stats_v2", BPF_MAP_TYPE_PERCPU_ARRAY, sizeof(UdpDsrStatsValue), 1,
      0}}};

[[noreturn]] void throwMapSystemError(const std::string& operation) {
  int error = errno;
  throw std::runtime_error(operation + " errno=" + std::to_string(error) +
                           ": " + std::strerror(error));
}

int validateMapDescriptor(bpf_object* object, const MapSpec& spec) {
  auto* map = bpf_object__find_map_by_name(object, spec.mapName);
  if (!map) throw std::runtime_error("缺少 map " + std::string(spec.mapName));

  int fd = bpf_map__fd(map);

  bpf_map_info info{};
  uint32_t infoSize = sizeof(info);
  if (bpf_obj_get_info_by_fd(fd, &info, &infoSize))
    throwMapSystemError("读取 map metadata " + std::string(spec.mapName));

  if (std::strcmp(info.name, spec.mapName) ||
      info.type != static_cast<uint32_t>(spec.mapType) ||
      info.key_size != sizeof(uint32_t) || info.value_size != spec.valueSize ||
      info.max_entries != spec.maxEntries || info.map_flags != spec.mapFlags)
    throw std::runtime_error("已加载 map metadata 不匹配：" +
                             std::string(spec.mapName));

  return fd;
}
}  // namespace

void DsrMapStore::validateDsrMapObject(bpf_object* object) {
  size_t count = 0;
  bpf_map* map;
  bpf_object__for_each_map(map, object) {
    ++count;
    bool matched = false;
    for (const auto& spec : kMapSpecs) {
      if (std::strcmp(bpf_map__name(map), spec.mapName)) continue;
      matched = bpf_map__type(map) == spec.mapType &&
                bpf_map__key_size(map) == sizeof(uint32_t) &&
                bpf_map__value_size(map) == spec.valueSize &&
                bpf_map__max_entries(map) == spec.maxEntries &&
                bpf_map__map_flags(map) == spec.mapFlags;
      break;
    }

    if (!matched)
      throw std::runtime_error("map 白名单不匹配：" +
                               std::string(bpf_map__name(map)));
  }

  if (count != kMapSpecs.size())
    throw std::runtime_error("maps 模式必须恰好包含三个配置/统计 maps");

  for (const auto& spec : kMapSpecs)
    if (!bpf_object__find_map_by_name(object, spec.mapName))
      throw std::runtime_error("缺少 map " + std::string(spec.mapName));
}

DsrMapStore::DsrMapStore(bpf_object* object)
    : configFd_(validateMapDescriptor(object, kMapSpecs[0])),
      backendFd_(validateMapDescriptor(object, kMapSpecs[1])),
      statsFd_(validateMapDescriptor(object, kMapSpecs[2])) {}

void DsrMapStore::writeBackend(uint32_t index,
                               const UdpDsrBackendValue& value) {
  if (bpf_map_update_elem(backendFd_, &index, &value, BPF_ANY))
    throwMapSystemError("写入 backend index=" + std::to_string(index));
}

void DsrMapStore::writeConfig(const UdpDsrConfigValue& value) {
  uint32_t key = 0;
  if (bpf_map_update_elem(configFd_, &key, &value, BPF_ANY))
    throwMapSystemError("写入 cfg");
}

UdpDsrBackendValue DsrMapStore::readBackend(uint32_t index) {
  UdpDsrBackendValue value{};
  if (bpf_map_lookup_elem(backendFd_, &index, &value))
    throwMapSystemError("回读 backend index=" + std::to_string(index));
  return value;
}

UdpDsrConfigValue DsrMapStore::readConfig() {
  uint32_t key = 0;
  UdpDsrConfigValue value{};
  if (bpf_map_lookup_elem(configFd_, &key, &value))
    throwMapSystemError("回读 cfg");
  return value;
}

void DsrMapStore::freezeBackends() {
  if (bpf_map_freeze(backendFd_)) throwMapSystemError("freeze backends");
}

void DsrMapStore::freezeConfig() {
  if (bpf_map_freeze(configFd_)) throwMapSystemError("freeze cfg");
}

UdpDsrStatsValue DsrMapStore::readDsrStatistics() const {
  int possibleCpus = libbpf_num_possible_cpus();
  if (possibleCpus <= 0 ||
      static_cast<size_t>(possibleCpus) >
          std::numeric_limits<size_t>::max() / sizeof(UdpDsrStatsValue))
    throw std::runtime_error("无效 possible CPU 数量");

  std::vector<UdpDsrStatsValue> values(static_cast<size_t>(possibleCpus));
  uint32_t key = 0;
  if (bpf_map_lookup_elem(statsFd_, &key, values.data()))
    throwMapSystemError("读取 stats");

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
