#include "control/XdpConfigSync.h"

#include <arpa/inet.h>
#include <bpf/libbpf.h>

#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class FakeMaps final : public l4lb::xdp::ConfigMapAccess {
 public:
  std::array<XdpBackendValue, 64> backends{};
  XdpConfigValue config{};
  int operations = 0;
  int failAt = -1;
  int corruptIndex = -1;
  bool corruptConfig = false;
  bool backendFrozen = false;
  bool configFrozen = false;

  void step() {
    if (operations++ == failAt) throw std::runtime_error("injected failure");
  }

  void writeBackend(uint32_t index, const XdpBackendValue& value) override {
    require(operations == static_cast<int>(index), "backend write ordering");
    step();
    backends.at(index) = value;
  }

  void writeConfig(const XdpConfigValue& value) override {
    require(operations == 64, "config before full backend write");
    step();
    config = value;
  }

  XdpConfigValue readConfig() override {
    require(operations == 65, "config read ordering");
    step();
    auto value = config;
    if (corruptConfig) ++value.schemaVersion;
    return value;
  }

  XdpBackendValue readBackend(uint32_t index) override {
    require(operations == static_cast<int>(66 + index),
            "backend read ordering");
    step();
    auto value = backends.at(index);
    if (static_cast<int>(index) == corruptIndex) ++value.reserved;
    return value;
  }

  void freezeBackends() override {
    require(operations == 130, "freeze before readback");
    step();
    backendFrozen = true;
  }

  void freezeConfig() override {
    require(backendFrozen && operations == 131, "config freeze ordering");
    step();
    configFrozen = true;
  }
};

void testEndpoints() {
  using l4lb::control::parseXdpBackends;
  auto values = parseXdpBackends({"127.0.0.1:65535", "10.2.3.4:80"});
  const unsigned char expected[8] = {10, 2, 3, 4, 0, 80, 0, 0};
  require(std::memcmp(&values[1], expected, 8) == 0, "network byte order");
  for (const auto& endpoint :
       {"", "localhost:80", "::1:80", "0.0.0.0:80", "224.0.0.1:80",
        "239.255.255.255:80", "255.255.255.255:80", "1.2.3.4:0",
        "1.2.3.4:65536", "1.2.3.4:-1", "1.2.3.4:+1", "1.2.3.4:80x",
        "1.2.3.4:", "01.2.3.4:80"}) {
    bool rejected = false;
    try {
      parseXdpBackends({endpoint});
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected, "invalid endpoint accepted");
  }
  bool duplicateRejected = false;
  try {
    parseXdpBackends({"1.2.3.4:80", "1.2.3.4:080"});
  } catch (const std::invalid_argument&) {
    duplicateRejected = true;
  }
  require(duplicateRejected, "duplicate accepted");
  std::vector<std::string> endpoints;
  for (int index = 0; index < 64; ++index)
    endpoints.push_back("10.0.0.1:" + std::to_string(index + 1));
  require(parseXdpBackends(endpoints).size() == 64, "64 endpoints");
  endpoints.push_back("10.0.0.1:65");
  bool overflowRejected = false;
  try {
    parseXdpBackends(endpoints);
  } catch (const std::invalid_argument&) {
    overflowRejected = true;
  }
  require(overflowRejected, "65 endpoints accepted");
}

void testSynchronization() {
  auto backend = l4lb::control::parseXdpBackends({"10.2.3.4:8080"});
  for (int count : {0, 1, 64}) {
    FakeMaps maps;
    // Dirty unused slots must be explicitly reset as part of publication.
    for (auto& value : maps.backends) value.reserved = 9;
    std::vector<XdpBackendValue> values(static_cast<size_t>(count), backend[0]);
    l4lb::control::synchronizeXdpConfig(maps, values);
    require(maps.config.backendCount == static_cast<unsigned>(count) &&
                maps.config.schemaVersion == 1,
            "published config");
    require(maps.configFrozen && maps.operations == 132, "freeze missing");
    for (int index = count; index < 64; ++index)
      require(maps.backends[index].reserved == 0 &&
                  maps.backends[index].address == 0 &&
                  maps.backends[index].port == 0,
              "unused slot not zero");
  }
  for (int failAt = 0; failAt < 132; ++failAt) {
    FakeMaps maps;
    maps.failAt = failAt;
    bool rejected = false;
    try {
      l4lb::control::synchronizeXdpConfig(maps, backend);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    require(rejected && maps.operations == failAt + 1 && !maps.configFrozen,
            "failure did not stop sync");
  }
  for (int index = -1; index < 64; ++index) {
    FakeMaps maps;
    maps.corruptConfig = index == -1;
    maps.corruptIndex = index;
    bool rejected = false;
    try {
      l4lb::control::synchronizeXdpConfig(maps, backend);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    require(rejected && !maps.backendFrozen, "readback mismatch accepted");
  }
}

void testStats() {
  std::array<uint64_t, 3> values{std::numeric_limits<uint64_t>::max(), 2, 8};
  auto bytes = std::as_bytes(std::span(values));
  require(l4lb::xdp::sumPassPackets(bytes, 3) == 9, "modular CPU sum");
  for (int cpus : {-1, 0, 2, 4}) {
    bool rejected = false;
    try {
      l4lb::xdp::sumPassPackets(bytes, cpus);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    require(rejected, "invalid CPU buffer accepted");
  }
}

void testMetadata(const char* path) {
  bpf_object* object = bpf_object__open_file(path, nullptr);
  require(object && !libbpf_get_error(object), "open maps object");
  try {
    l4lb::xdp::MapStore::validateObject(object);
    auto* map = bpf_object__find_map_by_name(object, "l4lb_be_v1");
    require(map != nullptr, "backend map missing");
    bpf_map__set_max_entries(map, 63);
    bool rejected = false;
    try {
      l4lb::xdp::MapStore::validateObject(object);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    require(rejected, "wrong capacity accepted");
  } catch (...) {
    bpf_object__close(object);
    throw;
  }
  bpf_object__close(object);
}
}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "maps object required");
    testEndpoints();
    testSynchronization();
    testStats();
    testMetadata(argv[1]);
    std::cout << "XDP sync/ABI/endpoints/faults/stats/metadata passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
