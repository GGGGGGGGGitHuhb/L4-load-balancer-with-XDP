#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "xdp/RuntimeMapStore.h"

namespace {
using l4lb::xdp::RuntimeMapStore;
using l4lb::xdp::RuntimePublishError;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

size_t fdCount() {
  return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                       std::filesystem::directory_iterator());
}

UdpRuntimeSnapshot snapshot(uint64_t generation) {
  UdpRuntimeSnapshot value{};
  value.schemaVersion = 3;
  value.backendCount = generation % 2 ? 1 : 2;
  value.generation = generation;
  inet_pton(AF_INET, "198.19.0.100", &value.vipAddress);
  value.vipPort = htons(39001);
  for (uint32_t index = 0; index < value.backendCount; ++index) {
    auto& backend = value.backends[index];
    backend.ifindex = generation % 2 ? 11 : 22;
    backend.destinationMac[0] = backend.sourceMac[0] = 2;
    backend.destinationMac[5] = generation % 2 ? 0xa1 : 0xb2;
    backend.sourceMac[5] = generation % 2 ? 0xc1 : 0xd2;
  }
  return value;
}

std::vector<unsigned char> packet() {
  std::vector<unsigned char> data(60);
  data[0] = data[6] = 2;
  data[12] = 8;
  data[14] = 0x45;
  data[17] = 28;
  data[22] = 64;
  data[23] = 17;
  inet_pton(AF_INET, "198.18.0.1", data.data() + 26);
  inet_pton(AF_INET, "198.19.0.100", data.data() + 30);
  data[34] = 0x9c;
  data[35] = 0x40;
  data[36] = 0x98;
  data[37] = 0x59;
  data[39] = 8;
  uint32_t sum = 0;
  for (int i = 14; i < 34; i += 2) sum += (data[i] << 8) | data[i + 1];
  while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
  uint16_t check = ~sum;
  data[24] = check >> 8;
  data[25] = check & 255;
  return data;
}

void runPacket(int programFd, bool either, uint64_t generation,
               uint32_t action = XDP_REDIRECT) {
  auto input = packet();
  std::vector<unsigned char> output(256);
  bpf_test_run_opts options{};
  options.sz = sizeof(options);
  options.data_in = input.data();
  options.data_size_in = input.size();
  options.data_out = output.data();
  options.data_size_out = output.size();
  options.repeat = 1;
  require(!bpf_prog_test_run_opts(programFd, &options),
          "kernel test-run syscall");
  require(options.retval == action,
          "unexpected action / torn snapshot diagnostic");
  require(options.data_size_out == input.size(), "frame size changed");
  require(
      !std::memcmp(input.data() + 12, output.data() + 12, input.size() - 12),
      "L3 bytes changed");
  if (action == XDP_REDIRECT) {
    auto a = snapshot(1).backends[0];
    auto b = snapshot(2).backends[0];
    const auto& expected = generation % 2 ? a : b;
    bool matchesA = !std::memcmp(output.data(), a.destinationMac, 6) &&
                    !std::memcmp(output.data() + 6, a.sourceMac, 6);
    bool matchesB = !std::memcmp(output.data(), b.destinationMac, 6) &&
                    !std::memcmp(output.data() + 6, b.sourceMac, 6);
    require(either ? matchesA || matchesB
                   : !std::memcmp(output.data(), expected.destinationMac, 6) &&
                         !std::memcmp(output.data() + 6, expected.sourceMac, 6),
            "MAC must be complete A or B");
  } else {
    require(!std::memcmp(input.data(), output.data(), input.size()),
            "nonredirect modified frame");
  }
}

uint32_t activeId(int outerFd) {
  uint32_t key = 0, id = 0;
  require(!bpf_map_lookup_elem(outerFd, &key, &id), "outer ID lookup");
  return id;
}

void waitRetired(const std::vector<uint32_t>& ids) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    bool present = false;
    for (auto id : ids) {
      int fd = bpf_map_get_fd_by_id(id);
      if (fd >= 0) {
        close(fd);
        present = true;
      } else
        require(errno == ENOENT, "retirement query failed");
    }
    if (!present) return;
    require(std::chrono::steady_clock::now() < deadline, "retired map leaked");
    std::this_thread::yield();
  }
}

void readConcurrentPackets(int programFd, std::atomic<bool>& running,
                           std::atomic<unsigned>& packets,
                           std::exception_ptr& error) {
  try {
    while (running.load()) {
      runPacket(programFd, true, 0);
      ++packets;
    }
  } catch (...) {
    error = std::current_exception();
    running = false;
  }
}

void verify(const char* path) {
  size_t initialFds = fdCount();
  std::vector<uint32_t> retired;
  bpf_object* object = bpf_object__open_file(path, nullptr);
  require(object && !libbpf_get_error(object), "open object");
  try {
    RuntimeMapStore::validateObject(object);
    require(!bpf_object__load(object), "load object");
    int outerFd = bpf_object__find_map_fd_by_name(object, "l4lb_active_v3");
    int programFd =
        bpf_program__fd(bpf_object__find_program_by_name(object, "xdp_udp_rt"));
    {
      RuntimeMapStore store(object);
      store.publish(snapshot(1));
      runPacket(programFd, false, 1);
      uint32_t oldId = activeId(outerFd);
      size_t stableFds = fdCount();
      for (const char* fault : {"create", "write", "read", "mismatch", "freeze",
                                "metadata", "outer-update"}) {
        setenv("L4LB_RUNTIME_FAULT", fault, 1);
        bool rejected = false;
        try {
          store.publish(snapshot(2));
        } catch (const RuntimePublishError& error) {
          require(!error.committed(), "precommit mislabeled");
          rejected = true;
        } catch (const std::exception&) {
          rejected = true;
        }
        unsetenv("L4LB_RUNTIME_FAULT");
        require(
            rejected && store.generation() == 1 && activeId(outerFd) == oldId,
            "precommit failure changed active snapshot");
        require(fdCount() == stableFds, "candidate fd leaked");
        runPacket(programFd, false, 1);
      }
      std::atomic<bool> running{true};
      std::atomic<unsigned> packets{0};
      std::exception_ptr readerError;
      std::thread reader([&] {
        readConcurrentPackets(programFd, running, packets, readerError);
      });
      try {
        for (uint64_t generation = 2; generation <= 101; ++generation) {
          retired.push_back(activeId(outerFd));
          store.publish(snapshot(generation));
          runPacket(programFd, false, generation);
        }
      } catch (...) {
        running = false;
        reader.join();
        throw;
      }
      running = false;
      reader.join();
      if (readerError) std::rethrow_exception(readerError);
      require(packets > 0, "concurrent kernel reader did not execute");
      require(fdCount() == stableFds, "published fd count grew");
      waitRetired(retired);
      for (const char* fault : {"post-read", "post-mismatch"}) {
        auto generation = store.generation() + 1;
        oldId = activeId(outerFd);
        setenv("L4LB_RUNTIME_FAULT", fault, 1);
        bool committed = false;
        try {
          store.publish(snapshot(generation));
        } catch (const RuntimePublishError& error) {
          committed = error.committed();
        }
        unsetenv("L4LB_RUNTIME_FAULT");
        require(committed && store.generation() == generation &&
                    activeId(outerFd) != oldId,
                "postcommit failure pretended rollback");
        runPacket(programFd, false, generation);
      }
      auto empty = snapshot(store.generation() + 1);
      empty.backendCount = 0;
      std::memset(empty.backends, 0, sizeof(empty.backends));
      store.publish(empty);
      runPacket(programFd, false, 0, XDP_DROP);
      auto stats = store.readStats();
      require(stats.noBackendPackets == 1 && stats.dropPackets == 1 &&
                  !stats.invalidConfigPackets,
              "v3 empty active must DROP/count");
      retired.push_back(activeId(outerFd));
      std::cout << "kernel_object=" << path
                << " publishes=104 concurrent_packets=" << packets
                << " precommit_failures=7 postcommit_failures=2 PASS\n";
    }
    bpf_object__close(object);
  } catch (...) {
    bpf_object__close(object);
    throw;
  }
  waitRetired(retired);
  require(fdCount() == initialFds, "object fd leaked");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc >= 2, "usage: RuntimeMapStoreKernel_test OBJECT...");
    require(getenv("LD_PRELOAD") != nullptr, "fault library required");
    for (int index = 1; index < argc; ++index) verify(argv[index]);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
