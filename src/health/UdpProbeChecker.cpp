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

class UdpProbeSocketTransport final : public ProbeTransport {
 public:
  int openProbeSocket(const ProbeTarget& target) override {
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

  void closeProbeSocket(int handle) noexcept override { ::close(handle); }

  ProbeIoResult sendProbePacket(int handle,
                                std::span<const uint8_t> packet) override {
    auto bytes = ::send(handle, packet.data(), packet.size(), MSG_NOSIGNAL);
    return {static_cast<int>(bytes), bytes < 0 ? errno : 0};
  }

  ProbeIoResult receiveProbePacket(int handle,
                                   std::span<uint8_t> buffer) override {
    // MSG_TRUNC 返回原始数据报长度，因此较长数据报中的匹配前缀
    // 不会被误认为完整的 24 字节 echo。
    auto bytes = ::recv(handle, buffer.data(), buffer.size(), MSG_TRUNC);
    return {static_cast<int>(bytes), bytes < 0 ? errno : 0};
  }
};

class SteadyProbeCompletionClock final : public ProbeCompletionClock {
 public:
  ProbeClock::time_point nowAtCompletion(ProbeClock::time_point) override {
    return ProbeClock::now();
  }
};

uint64_t createRandomProbeEpoch() {
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

std::shared_ptr<ProbeNonceSource> sharedProcessNonceSource() {
  // 所有 checker（包括尚未发布的候选）共享这个进程范围的
  // 序号。控制面仅从一个线程调用它。
  static auto source =
      std::make_shared<ProbeNonceSequence>(createRandomProbeEpoch());
  return source;
}

bool isLocalProbeError(int error) {
  return error == ENOBUFS || error == ENOMEM || error == EMFILE ||
         error == ENFILE || error == EAGAIN || error == EINTR ||
         error == EADDRNOTAVAIL || error == EACCES || error == EPERM;
}

void validateProbeIoResult(const ProbeIoResult& result) {
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

ProbePacket ProbeNonceSequence::generateProbePacket() {
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
                           : std::make_shared<UdpProbeSocketTransport>()),
      nonceSource_(nonceSource ? std::move(nonceSource)
                               : sharedProcessNonceSource()),
      completionClock_(completionClock
                           ? std::move(completionClock)
                           : std::make_shared<SteadyProbeCompletionClock>()) {
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

      // 在打开之前登记所有权，避免分配失败遗留已打开的 fd。
      probes_.push_back(std::move(probe));
      probes_.back().handle = transport_->openProbeSocket(target);
      if (probes_.back().handle < 0)
        throw std::runtime_error("probe transport returned invalid handle");
    }
  } catch (...) {
    cancelAllProbes();
    throw;
  }
}

UdpProbeChecker::~UdpProbeChecker() { cancelAllProbes(); }

void UdpProbeChecker::cancelAllProbes() noexcept {
  for (auto& probe : probes_) {
    probe.pending = false;
    probe.token.fill(0);

    if (probe.handle >= 0) transport_->closeProbeSocket(probe.handle);
    probe.handle = -1;
  }
}

void UdpProbeChecker::completeProbeResult(
    Probe& probe, bool success, const char* reason, int error,
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

std::vector<ProbeTransition> UdpProbeChecker::pollProbeTransitions(
    ProbeClock::time_point pollTime) {
  std::vector<ProbeTransition> transitions;

  try {
    if (hasPolled_ && pollTime < lastPollTime_)
      throw std::runtime_error("probe clock moved backwards");

    lastPollTime_ = pollTime;
    hasPolled_ = true;
    lastReceiveEventCount_ = 0;

    for (auto& probe : probes_) {
      if (probe.handle < 0)
        throw std::runtime_error("probe checker has terminated");

      // 截止时间优先于已排队的回复，包括时间相等的情形。
      if (probe.pending && pollTime >= probe.deadline)
        completeProbeResult(probe, false, "timeout", ETIMEDOUT, transitions);
      if (probe.pending || pollTime < probe.next) continue;

      probe.token = nonceSource_->generateProbePacket();
      probe.next = pollTime + kInterval;
      probe.deadline = pollTime + kTimeout;

      auto sent = transport_->sendProbePacket(probe.handle, probe.token);
      validateProbeIoResult(sent);
      if (sent.bytes != static_cast<int>(probe.token.size())) {
        if (sent.bytes >= 0) throw std::runtime_error("partial UDP probe send");
        completeProbeResult(
            probe, false,
            isLocalProbeError(sent.error) ? "local_error" : "target_error",
            sent.error, transitions);
      } else {
        probe.pending = true;
      }
    }

    // 在目标间轮转，避免嘈杂对端独占每次轮询。
    size_t count = probes_.size();
    for (size_t offset = 0;
         offset < count && lastReceiveEventCount_ < kReceiveBudget; ++offset) {
      size_t index = (receiveCursor_ + offset) % count;
      auto& probe = probes_[index];
      while (probe.pending && lastReceiveEventCount_ < kReceiveBudget) {
        std::array<uint8_t, 25> reply{};
        auto received = transport_->receiveProbePacket(probe.handle, reply);
        validateProbeIoResult(received);

        bool wouldBlock = received.bytes < 0 && (received.error == EAGAIN ||
                                                 received.error == EWOULDBLOCK);
        if (!wouldBlock) ++lastReceiveEventCount_;

        auto completedAt = completionClock_->nowAtCompletion(pollTime);
        if (completedAt < pollTime)
          throw std::runtime_error("probe completion clock moved backwards");
        if (completedAt >= probe.deadline) {
          completeProbeResult(probe, false, "timeout", ETIMEDOUT, transitions);
          break;
        }

        if (wouldBlock) break;
        if (received.bytes < 0) {
          if (received.error == EINTR) continue;
          completeProbeResult(probe, false,
                              isLocalProbeError(received.error)
                                  ? "local_error"
                                  : "target_error",
                              received.error, transitions);
          break;
        }

        if (received.bytes == static_cast<int>(probe.token.size()) &&
            std::equal(probe.token.begin(), probe.token.end(), reply.begin()))
          completeProbeResult(probe, true, "success", 0, transitions);
      }
    }

    if (count) receiveCursor_ = (receiveCursor_ + 1) % count;
  } catch (...) {
    cancelAllProbes();
    throw;
  }

  return transitions;
}

std::vector<ProbeSnapshot> UdpProbeChecker::copyProbeSnapshots() const {
  std::vector<ProbeSnapshot> result;
  result.reserve(probes_.size());
  for (const auto& probe : probes_) result.push_back(probe.snapshot);

  return result;
}

std::vector<ProbeTarget> UdpProbeChecker::copyHealthyProbeTargets() const {
  std::vector<ProbeTarget> result;
  for (const auto& probe : probes_)
    if (probe.snapshot.state == ProbeState::kHealthy)
      result.push_back(probe.snapshot.target);

  return result;
}

ProbeClock::time_point UdpProbeChecker::nextProbeWakeup() const {
  auto result = ProbeClock::time_point::max();
  for (const auto& probe : probes_)
    result = std::min(result, probe.pending ? probe.deadline : probe.next);

  return result;
}

size_t UdpProbeChecker::inFlightProbeCount() const {
  size_t count = 0;
  for (const auto& probe : probes_)
    if (probe.pending) ++count;

  return count;
}
}  // namespace l4lb::health
