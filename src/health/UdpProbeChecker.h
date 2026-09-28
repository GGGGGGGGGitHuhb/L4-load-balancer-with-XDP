#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace l4lb::health {

using ProbeClock = std::chrono::steady_clock;
using ProbePacket = std::array<uint8_t, 24>;

enum class ProbeState { kUnknown, kHealthy, kUnhealthy };
const char* probeStateName(ProbeState state);

/** Addresses and ports use network byte order; MACs participate in identity. */
struct ProbeTarget {
  std::string id;
  std::string interfaceName;
  uint32_t ifindex = 0;
  std::array<uint8_t, 6> sourceMac{};
  std::array<uint8_t, 6> destinationMac{};
  uint32_t probeAddress = 0;
  uint16_t probePort = 0;
};

bool sameProbeIdentity(const ProbeTarget& first, const ProbeTarget& second);

struct ProbeSnapshot {
  ProbeTarget target;
  ProbeState state = ProbeState::kUnknown;
  unsigned consecutiveSuccesses = 0;
  unsigned consecutiveFailures = 0;
};

struct ProbeTransition {
  std::string id;
  ProbeState from;
  ProbeState to;
  std::string reason;
  int error = 0;
};

/** Nonblocking transport seam; handles belong exclusively to one checker. */
struct ProbeIoResult {
  int bytes = -1;
  int error = 0;
};

class ProbeTransport {
 public:
  virtual ~ProbeTransport() = default;
  virtual int open(const ProbeTarget& target) = 0;
  virtual void close(int handle) noexcept = 0;
  virtual ProbeIoResult send(int handle, std::span<const uint8_t> packet) = 0;
  virtual ProbeIoResult receive(int handle, std::span<uint8_t> buffer) = 0;
};

class ProbeNonceSource {
 public:
  virtual ~ProbeNonceSource() = default;
  virtual ProbePacket next() = 0;
};

/** Completion timestamp. Tests can project logical tick time deterministically.
 */
class ProbeCompletionClock {
 public:
  virtual ~ProbeCompletionClock() = default;
  virtual ProbeClock::time_point nowAtCompletion(
      ProbeClock::time_point tickTime) = 0;
};

/** Pure encoder with checked monotonic sequence, also usable in time tests. */
class ProbeNonceSequence final : public ProbeNonceSource {
 public:
  explicit ProbeNonceSequence(uint64_t epoch, uint64_t lastSequence = 0);
  ProbePacket next() override;

 private:
  uint64_t epoch_;
  uint64_t lastSequence_;
};

/** Single-threaded UDP echo health model. No callbacks, BPF access or logging.
 * Construction prepares candidate sockets without touching the old checker.
 * Commit by replacing the old owner; destruction cancels tokens before close.
 */
class UdpProbeChecker {
 public:
  explicit UdpProbeChecker(
      const std::vector<ProbeTarget>& targets,
      const std::vector<ProbeSnapshot>& previous = {},
      std::shared_ptr<ProbeTransport> transport = {},
      std::shared_ptr<ProbeNonceSource> nonceSource = {},
      std::shared_ptr<ProbeCompletionClock> completionClock = {});
  ~UdpProbeChecker();
  UdpProbeChecker(const UdpProbeChecker&) = delete;
  UdpProbeChecker& operator=(const UdpProbeChecker&) = delete;

  std::vector<ProbeTransition> tick(ProbeClock::time_point now);
  std::vector<ProbeSnapshot> snapshots() const;
  std::vector<ProbeTarget> healthyTargets() const;
  ProbeClock::time_point nextWakeup() const;
  size_t inFlight() const;

  size_t lastReceiveEvents() const { return lastReceiveEvents_; }

 private:
  struct Probe {
    ProbeSnapshot snapshot;
    int handle = -1;
    bool pending = false;
    ProbePacket token{};
    ProbeClock::time_point next{};
    ProbeClock::time_point deadline{};
  };

  void finish(Probe& probe, bool success, const char* reason, int error,
              std::vector<ProbeTransition>& transitions);
  void cancelAll() noexcept;

  std::shared_ptr<ProbeTransport> transport_;
  std::shared_ptr<ProbeNonceSource> nonceSource_;
  std::shared_ptr<ProbeCompletionClock> completionClock_;
  std::vector<Probe> probes_;
  ProbeClock::time_point lastTick_{};
  bool ticked_ = false;
  size_t receiveCursor_ = 0;
  size_t lastReceiveEvents_ = 0;
};

}  // namespace l4lb::health
