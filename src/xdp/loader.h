#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "MapSchema.h"
#include "control/DsrConfigSync.h"

struct bpf_object;

namespace l4lb::xdp {

/** Explicit hook selection; never silently fall back from native to generic. */
enum class Mode { Generic, Native };

/** Mutually exclusive object and configuration ABI. */
enum class Profile { kLegacy, kMapsV1, kUdpDsrV2 };

const char* mode_name(Mode mode);
int interface_index(const std::string& device);

/** Owns a loaded object and its attachment until explicit cleanup or
 * destruction. */
class Attachment {
 public:
  Attachment() = default;
  ~Attachment();
  Attachment(const Attachment&) = delete;
  Attachment& operator=(const Attachment&) = delete;

  void load(const std::string& path, Profile profile = Profile::kLegacy,
            const std::vector<XdpBackendValue>& backends = {},
            const control::DsrConfiguration& dsr = {});
  UdpDsrStatsValue readDsrStats() const;
  uint64_t readPassPackets() const;
  void attach(int ifindex, Mode mode);
  void detach();

  uint32_t program_id() const { return program_id_; }

 private:
  std::vector<char> object_bytes_;
  bpf_object* object_ = nullptr;
  int program_fd_ = -1;
  uint32_t program_id_ = 0;
  int ifindex_ = 0;
  Mode mode_ = Mode::Generic;
  bool attached_ = false;
  Profile profile_ = Profile::kLegacy;
};

/** Atomically detach only the requested program, or succeed if already absent.
 */
void detach_program(int ifindex, Mode mode, uint32_t expected_id);

}  // namespace l4lb::xdp
