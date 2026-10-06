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

/** 地址和端口使用网络字节序；MAC 参与身份匹配。 */
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

/** 非阻塞传输测试缝；句柄仅由一个 checker 独占。 */
struct ProbeIoResult {
  int bytes = -1;
  int error = 0;
};

class ProbeTransport {
 public:
  virtual ~ProbeTransport() = default;

  virtual int openProbeSocket(const ProbeTarget& target) = 0;

  virtual void closeProbeSocket(int handle) noexcept = 0;

  virtual ProbeIoResult sendProbePacket(int handle,
                                        std::span<const uint8_t> packet) = 0;

  virtual ProbeIoResult receiveProbePacket(int handle,
                                           std::span<uint8_t> buffer) = 0;
};

class ProbeNonceSource {
 public:
  virtual ~ProbeNonceSource() = default;

  virtual ProbePacket generateProbePacket() = 0;
};

/** 完成时间戳；测试可以确定性地投射逻辑轮询时间。 */
class ProbeCompletionClock {
 public:
  virtual ~ProbeCompletionClock() = default;

  virtual ProbeClock::time_point nowAtCompletion(
      ProbeClock::time_point pollTime) = 0;
};

/** 检查单调序号的纯编码器，也可用于时间测试。 */
class ProbeNonceSequence final : public ProbeNonceSource {
 public:
  explicit ProbeNonceSequence(uint64_t epoch, uint64_t lastSequence = 0);

  ProbePacket generateProbePacket() override;

 private:
  uint64_t epoch_;
  uint64_t lastSequence_;
};

/** 单线程 UDP echo 健康模型，不使用回调、BPF 访问或日志。
 * 构造时准备候选 socket，不触碰旧 checker。
 * 通过替换旧 owner 提交；析构时先取消 token 再关闭。
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

  std::vector<ProbeTransition> pollProbeTransitions(
      ProbeClock::time_point pollTime);

  std::vector<ProbeSnapshot> copyProbeSnapshots() const;
  std::vector<ProbeTarget> copyHealthyProbeTargets() const;

  ProbeClock::time_point nextProbeWakeup() const;
  size_t inFlightProbeCount() const;

  size_t lastReceiveEventCount() const { return lastReceiveEventCount_; }

 private:
  struct Probe {
    ProbeSnapshot snapshot;

    int handle = -1;
    bool pending = false;
    ProbePacket token{};

    ProbeClock::time_point next{};
    ProbeClock::time_point deadline{};
  };

  void completeProbeResult(Probe& probe, bool success, const char* reason,
                           int error,
                           std::vector<ProbeTransition>& transitions);
  void cancelAllProbes() noexcept;

  std::shared_ptr<ProbeTransport> transport_;
  std::shared_ptr<ProbeNonceSource> nonceSource_;
  std::shared_ptr<ProbeCompletionClock> completionClock_;

  std::vector<Probe> probes_;

  ProbeClock::time_point lastPollTime_{};
  bool hasPolled_ = false;

  size_t receiveCursor_ = 0;
  size_t lastReceiveEventCount_ = 0;
};

}  // namespace l4lb::health
