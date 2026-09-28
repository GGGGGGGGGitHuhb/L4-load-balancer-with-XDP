#include "UdpProbeChecker.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>

namespace l4lb::health {
namespace {
constexpr auto kInterval = std::chrono::seconds(1);
constexpr auto kTimeout = std::chrono::milliseconds(500);
constexpr size_t kReceiveBudget = 256;

[[noreturn]] void socketFailure(const char* operation, int error) {
  throw std::system_error(error, std::generic_category(), operation);
}

class SocketTransport final : public ProbeTransport {
 public:
  int open(const ProbeTarget& target) override {
    if (if_nametoindex(target.interfaceName.c_str()) != target.ifindex)
      throw std::runtime_error("probe interface identity changed");
    int handle =
        ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (handle < 0) socketFailure("probe socket", errno);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = target.probeAddress;
    address.sin_port = target.probePort;
    if (setsockopt(handle, SOL_SOCKET, SO_BINDTODEVICE,
                   target.interfaceName.c_str(),
                   target.interfaceName.size() + 1) ||
        ::connect(handle, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address))) {
      int error = errno;
      ::close(handle);
      socketFailure("prepare bound UDP probe", error);
    }
    return handle;
  }

  void close(int handle) noexcept override { ::close(handle); }

  ProbeIoResult send(int handle, std::span<const uint8_t> packet) override {
    auto bytes = ::send(handle, packet.data(), packet.size(), MSG_NOSIGNAL);
    return {static_cast<int>(bytes), bytes < 0 ? errno : 0};
  }

  ProbeIoResult receive(int handle, std::span<uint8_t> buffer) override {
    // MSG_TRUNC reports the original datagram size, so a long matching prefix
    // cannot be mistaken for a complete 24-byte echo.
    auto bytes = ::recv(handle, buffer.data(), buffer.size(), MSG_TRUNC);
    return {static_cast<int>(bytes), bytes < 0 ? errno : 0};
  }
};

class SteadyCompletionClock final : public ProbeCompletionClock {
 public:
  ProbeClock::time_point nowAtCompletion(ProbeClock::time_point) override {
    return ProbeClock::now();
  }
};

uint64_t randomEpoch() {
  uint64_t epoch = 0;
  size_t completed = 0;
  auto* bytes = reinterpret_cast<uint8_t*>(&epoch);
  while (completed < sizeof(epoch)) {
    auto count = getrandom(bytes + completed, sizeof(epoch) - completed, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0)
      socketFailure("probe random epoch", count < 0 ? errno : EIO);
    completed += static_cast<size_t>(count);
  }
  return epoch;
}

std::shared_ptr<ProbeNonceSource> processNonceSource() {
  // All checkers, including unpublished candidates, share this process-wide
  // sequence. The control plane invokes it from one thread only.
  static auto source = std::make_shared<ProbeNonceSequence>(randomEpoch());
  return source;
}

bool localError(int error) {
  return error == ENOBUFS || error == ENOMEM || error == EMFILE ||
         error == ENFILE || error == EAGAIN || error == EINTR ||
         error == EADDRNOTAVAIL || error == EACCES || error == EPERM;
}

void validateIo(const ProbeIoResult& result) {
  if ((result.bytes >= 0 && result.error) ||
      (result.bytes < 0 && !result.error) || result.error == EBADF ||
      result.error == ENOTSOCK || result.error == EFAULT ||
      result.error == EINVAL)
    throw std::runtime_error("probe transport invariant failed");
}
}  // namespace

const char* probeStateName(ProbeState state) {
  switch (state) {
    case ProbeState::kUnknown:
      return "Unknown";
    case ProbeState::kHealthy:
      return "Healthy";
    case ProbeState::kUnhealthy:
      return "Unhealthy";
  }
  throw std::runtime_error("invalid probe health state");
}

bool sameProbeIdentity(const ProbeTarget& first, const ProbeTarget& second) {
  return first.id == second.id && first.ifindex == second.ifindex &&
         first.sourceMac == second.sourceMac &&
         first.destinationMac == second.destinationMac &&
         first.probeAddress == second.probeAddress &&
         first.probePort == second.probePort;
}

ProbeNonceSequence::ProbeNonceSequence(uint64_t epoch, uint64_t lastSequence)
    : epoch_(epoch), lastSequence_(lastSequence) {}

ProbePacket ProbeNonceSequence::next() {
  if (lastSequence_ == std::numeric_limits<uint64_t>::max())
    throw std::runtime_error("probe sequence exhausted");
  uint64_t sequence = ++lastSequence_;
  ProbePacket packet{'L', '4', 'L', 'B', 'H', 'C', '0', '1'};
  for (unsigned index = 0; index < 8; ++index) {
    packet[8 + index] = static_cast<uint8_t>(epoch_ >> (56 - 8 * index));
    packet[16 + index] = static_cast<uint8_t>(sequence >> (56 - 8 * index));
  }
  return packet;
}

UdpProbeChecker::UdpProbeChecker(
    const std::vector<ProbeTarget>& targets,
    const std::vector<ProbeSnapshot>& previous,
    std::shared_ptr<ProbeTransport> transport,
    std::shared_ptr<ProbeNonceSource> nonceSource,
    std::shared_ptr<ProbeCompletionClock> completionClock)
    : transport_(transport ? std::move(transport)
                           : std::make_shared<SocketTransport>()),
      nonceSource_(nonceSource ? std::move(nonceSource) : processNonceSource()),
      completionClock_(completionClock
                           ? std::move(completionClock)
                           : std::make_shared<SteadyCompletionClock>()) {
  if (targets.size() > 64)
    throw std::invalid_argument("probe target limit is 64");
  std::set<std::string> ids;
  for (const auto& target : targets) {
    if (target.id.empty() || !ids.insert(target.id).second || !target.ifindex ||
        target.interfaceName.empty() ||
        target.interfaceName.size() >= IF_NAMESIZE || !target.probeAddress ||
        !target.probePort)
      throw std::invalid_argument("invalid or duplicate probe target");
  }
  probes_.reserve(targets.size());
  try {
    for (const auto& target : targets) {
      Probe probe;
      probe.snapshot.target = target;
      for (const auto& old : previous) {
        if (!sameProbeIdentity(target, old.target)) continue;
        if (old.consecutiveSuccesses > 2 || old.consecutiveFailures > 3 ||
            (old.consecutiveSuccesses && old.consecutiveFailures))
          throw std::runtime_error("invalid imported probe counters");
        probeStateName(old.state);
        probe.snapshot.state = old.state;
        probe.snapshot.consecutiveSuccesses = old.consecutiveSuccesses;
        probe.snapshot.consecutiveFailures = old.consecutiveFailures;
        break;
      }
      // Insert ownership before opening: no allocation can orphan an open fd.
      probes_.push_back(std::move(probe));
      probes_.back().handle = transport_->open(target);
      if (probes_.back().handle < 0)
        throw std::runtime_error("probe transport returned invalid handle");
    }
  } catch (...) {
    cancelAll();
    throw;
  }
}

UdpProbeChecker::~UdpProbeChecker() { cancelAll(); }

void UdpProbeChecker::cancelAll() noexcept {
  for (auto& probe : probes_) {
    probe.pending = false;
    probe.token.fill(0);
    if (probe.handle >= 0) transport_->close(probe.handle);
    probe.handle = -1;
  }
}

void UdpProbeChecker::finish(Probe& probe, bool success, const char* reason,
                             int error,
                             std::vector<ProbeTransition>& transitions) {
  probe.pending = false;
  probe.token.fill(0);
  auto& snapshot = probe.snapshot;
  auto previous = snapshot.state;
  if (success) {
    snapshot.consecutiveFailures = 0;
    snapshot.consecutiveSuccesses =
        std::min(snapshot.consecutiveSuccesses + 1, 2U);
    if (snapshot.consecutiveSuccesses == 2)
      snapshot.state = ProbeState::kHealthy;
  } else {
    snapshot.consecutiveSuccesses = 0;
    snapshot.consecutiveFailures =
        std::min(snapshot.consecutiveFailures + 1, 3U);
    if (snapshot.consecutiveFailures == 3)
      snapshot.state = ProbeState::kUnhealthy;
  }
  if (previous != snapshot.state)
    transitions.push_back(
        {snapshot.target.id, previous, snapshot.state, reason, error});
}

std::vector<ProbeTransition> UdpProbeChecker::tick(ProbeClock::time_point now) {
  std::vector<ProbeTransition> transitions;
  try {
    if (ticked_ && now < lastTick_)
      throw std::runtime_error("probe clock moved backwards");
    lastTick_ = now;
    ticked_ = true;
    lastReceiveEvents_ = 0;
    for (auto& probe : probes_) {
      if (probe.handle < 0)
        throw std::runtime_error("probe checker has terminated");
      // Deadlines win over queued replies, including equality.
      if (probe.pending && now >= probe.deadline)
        finish(probe, false, "timeout", ETIMEDOUT, transitions);
      if (probe.pending || now < probe.next) continue;
      probe.token = nonceSource_->next();
      probe.next = now + kInterval;
      probe.deadline = now + kTimeout;
      auto sent = transport_->send(probe.handle, probe.token);
      validateIo(sent);
      if (sent.bytes != static_cast<int>(probe.token.size())) {
        if (sent.bytes >= 0) throw std::runtime_error("partial UDP probe send");
        finish(probe, false,
               localError(sent.error) ? "local_error" : "target_error",
               sent.error, transitions);
      } else {
        probe.pending = true;
      }
    }
    // Rotate between targets; a noisy peer cannot monopolize every tick.
    size_t count = probes_.size();
    for (size_t offset = 0;
         offset < count && lastReceiveEvents_ < kReceiveBudget; ++offset) {
      size_t index = (receiveCursor_ + offset) % count;
      auto& probe = probes_[index];
      while (probe.pending && lastReceiveEvents_ < kReceiveBudget) {
        std::array<uint8_t, 25> reply{};
        auto received = transport_->receive(probe.handle, reply);
        validateIo(received);
        bool wouldBlock = received.bytes < 0 && (received.error == EAGAIN ||
                                                 received.error == EWOULDBLOCK);
        if (!wouldBlock) ++lastReceiveEvents_;
        auto completedAt = completionClock_->nowAtCompletion(now);
        if (completedAt < now)
          throw std::runtime_error("probe completion clock moved backwards");
        if (completedAt >= probe.deadline) {
          finish(probe, false, "timeout", ETIMEDOUT, transitions);
          break;
        }
        if (wouldBlock) break;
        if (received.bytes < 0) {
          if (received.error == EINTR) continue;
          finish(probe, false,
                 localError(received.error) ? "local_error" : "target_error",
                 received.error, transitions);
          break;
        }
        if (received.bytes == static_cast<int>(probe.token.size()) &&
            std::equal(probe.token.begin(), probe.token.end(), reply.begin()))
          finish(probe, true, "success", 0, transitions);
      }
    }
    if (count) receiveCursor_ = (receiveCursor_ + 1) % count;
  } catch (...) {
    cancelAll();
    throw;
  }
  return transitions;
}

std::vector<ProbeSnapshot> UdpProbeChecker::snapshots() const {
  std::vector<ProbeSnapshot> result;
  result.reserve(probes_.size());
  for (const auto& probe : probes_) result.push_back(probe.snapshot);
  return result;
}

std::vector<ProbeTarget> UdpProbeChecker::healthyTargets() const {
  std::vector<ProbeTarget> result;
  for (const auto& probe : probes_)
    if (probe.snapshot.state == ProbeState::kHealthy)
      result.push_back(probe.snapshot.target);
  return result;
}

ProbeClock::time_point UdpProbeChecker::nextWakeup() const {
  auto result = ProbeClock::time_point::max();
  for (const auto& probe : probes_)
    result = std::min(result, probe.pending ? probe.deadline : probe.next);
  return result;
}

size_t UdpProbeChecker::inFlight() const {
  size_t count = 0;
  for (const auto& probe : probes_)
    if (probe.pending) ++count;
  return count;
}
}  // namespace l4lb::health
