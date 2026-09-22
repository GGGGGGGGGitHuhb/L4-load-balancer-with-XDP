#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "MapSchema.h"

struct bpf_object;

namespace l4lb::xdp {

/** Explicit hook selection; never silently fall back from native to generic. */
enum class Mode { Generic, Native };

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

  void load(const std::string& path, bool mapsMode = false,
            const std::vector<XdpBackendValue>& backends = {});
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
  bool mapsMode_ = false;
};

/** Atomically detach only the requested program, or succeed if already absent.
 */
void detach_program(int ifindex, Mode mode, uint32_t expected_id);

}  // namespace l4lb::xdp
