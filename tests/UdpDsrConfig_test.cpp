#include <arpa/inet.h>
#include <bpf/libbpf.h>

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "control/DsrConfigSync.h"
#include "xdp/loader.h"

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class FakeMaps final : public l4lb::xdp::DsrMapAccess {
 public:
  std::array<UdpDsrBackendValue, 64> backends{};
  UdpDsrConfigValue config{};
  int operations = 0;
  int failAt = -1;
  int corruptIndex = -1;
  bool corruptConfig = false;
  bool backendFrozen = false;
  bool configFrozen = false;

  void step() {
    if (operations++ == failAt) throw std::runtime_error("injected failure");
  }

  void writeBackend(uint32_t index, const UdpDsrBackendValue& value) override {
    require(operations == static_cast<int>(index), "backend order");
    step();
    backends.at(index) = value;
  }

  void writeConfig(const UdpDsrConfigValue& value) override {
    require(operations == 64, "config before all slots");
    step();
    config = value;
  }

  UdpDsrConfigValue readConfig() override {
    require(operations == 65, "read config order");
    step();
    auto value = config;
    if (corruptConfig) ++value.reserved;
    return value;
  }

  UdpDsrBackendValue readBackend(uint32_t index) override {
    require(operations == static_cast<int>(66 + index), "read backend order");
    step();
    auto value = backends.at(index);
    if (static_cast<int>(index) == corruptIndex) ++value.sourceMac[5];
    return value;
  }

  void freezeBackends() override {
    require(operations == 130, "freeze before complete readback");
    step();
    backendFrozen = true;
  }

  void freezeConfig() override {
    require(backendFrozen && operations == 131, "config freeze order");
    step();
    configFrozen = true;
  }
};

l4lb::control::DsrConfiguration configuration(unsigned count) {
  l4lb::control::DsrConfiguration result{
      {2, count, htonl(0x0a000001), htons(9000), 0}, {}};
  for (unsigned index = 0; index < count; ++index)
    result.backends.push_back(
        {index + 1,
         {2, 0, 0, 0, 0, static_cast<unsigned char>(index)},
         {2, 0, 0, 0, 1, 1}});
  return result;
}

void testSync() {
  for (unsigned count : {0U, 1U, 2U, 64U}) {
    FakeMaps maps;
    maps.backends.fill({123, {2, 1, 2, 3, 4, 5}, {2, 3, 4, 5, 6, 7}});
    l4lb::control::synchronizeDsrConfig(maps, configuration(count));
    require(maps.configFrozen && maps.backendFrozen && maps.operations == 132,
            "publish incomplete");
    UdpDsrBackendValue empty{};
    for (unsigned index = count; index < 64; ++index)
      require(std::memcmp(&maps.backends[index], &empty, sizeof(empty)) == 0,
              "unused slot not cleared");
  }
  for (int failure = 0; failure < 132; ++failure) {
    FakeMaps maps;
    maps.failAt = failure;
    bool rejected = false;
    try {
      l4lb::control::synchronizeDsrConfig(maps, configuration(2));
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    require(rejected && !maps.configFrozen, "failure published config");
  }
  for (int index = -1; index < 64; ++index) {
    FakeMaps maps;
    maps.corruptIndex = index;
    maps.corruptConfig = index == -1;
    bool rejected = false;
    try {
      l4lb::control::synchronizeDsrConfig(maps, configuration(2));
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    require(rejected && !maps.backendFrozen, "bad readback frozen");
  }
  auto invalid = configuration(2);
  invalid.config.backendCount = 1;
  FakeMaps maps;
  bool rejected = false;
  try {
    l4lb::control::synchronizeDsrConfig(maps, invalid);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected && maps.operations == 0, "invalid image writes maps");
}

void testLiterals() {
  for (const auto& vip :
       {"", "localhost:90", "0.0.0.0:90", "224.0.0.1:90", "255.255.255.255:90",
        "1.2.3.4:0", "1.2.3.4:65536", "01.2.3.4:90"}) {
    bool rejected = false;
    try {
      l4lb::control::parseDsrConfiguration("unused", vip, {});
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected, "invalid VIP accepted");
  }
  for (const auto& target :
       {"eth0", "eth0@00:00:00:00:00:00", "eth0@ff:ff:ff:ff:ff:ff",
        "eth0@01:00:00:00:00:01", "eth0@2:00:00:00:00:01",
        "eth0@02:00:00:00:00:gg", "eth0@02-00:00:00:00:01",
        "eth0@02:00:00:00:00:01x"}) {
    bool rejected = false;
    try {
      l4lb::control::parseDsrConfiguration("unused", "10.0.0.1:90", {target});
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected, "invalid target accepted");
  }
}

void testMetadata(const char* path) {
  for (const char* name : {"l4lb_cfg_v2", "l4lb_be_v2", "l4lb_stats_v2"}) {
    for (int mutation = 0; mutation < 6; ++mutation) {
      auto* object = bpf_object__open_file(path, nullptr);
      require(object && !libbpf_get_error(object), "open object");
      auto* map = bpf_object__find_map_by_name(object, name);
      require(map, "missing v2 map");
      if (mutation == 1) bpf_map__set_type(map, BPF_MAP_TYPE_HASH);
      if (mutation == 2) bpf_map__set_key_size(map, 8);
      if (mutation == 3) bpf_map__set_value_size(map, 128);
      if (mutation == 4) bpf_map__set_max_entries(map, 99);
      if (mutation == 5) bpf_map__set_map_flags(map, BPF_F_NO_PREALLOC);
      bool rejected = false;
      try {
        l4lb::xdp::DsrMapStore::validateObject(object);
      } catch (const std::runtime_error&) {
        rejected = true;
      }
      bpf_object__close(object);
      require(rejected == (mutation != 0), "metadata allowlist mismatch");
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "--reject") {
      try {
        l4lb::xdp::Attachment attachment;
        attachment.load(argv[2], l4lb::xdp::Profile::kUdpDsrV2);
      } catch (const std::runtime_error& error) {
        std::string message = error.what();
        require(message.find("白名单") != std::string::npos ||
                    message.find("三个") != std::string::npos ||
                    message.find("对象应仅包含") != std::string::npos,
                "object not rejected by pre-load allowlist");
        return 0;
      }
      throw std::runtime_error("invalid object accepted");
    }
    require(argc == 2, "object path required");
    testSync();
    testLiterals();
    testMetadata(argv[1]);
    std::cout << "DSR ABI, literals, 132 sync failures, 65 readback faults and "
                 "metadata PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
