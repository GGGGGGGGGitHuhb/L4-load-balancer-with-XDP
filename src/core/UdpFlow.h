#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "config/Config.h"

namespace l4lb {
/** 单 listener 的 UDP key；listener 端口和协议由实例隐含。 */
struct UdpFlowKey {
  Endpoint client;
  std::array<std::uint8_t, 4> localAddress;

  bool operator==(const UdpFlowKey&) const = default;
};

struct UdpFlowHash {
  std::size_t operator()(const UdpFlowKey& key) const noexcept {
    std::size_t value = key.client.port;
    for (auto byte : key.client.address) value = value * 131 + byte;
    for (auto byte : key.localAddress) value = value * 131 + byte;
    return value;
  }
};

/** UDP 成功发送零长包也算活动；收到或丢弃不更新。 */
struct UdpIdleDeadline {
  using Clock = std::chrono::steady_clock;

  Clock::time_point lastSubmissionTime;

  void recordSubmission(Clock::time_point now) { lastSubmissionTime = now; }

  bool expired(Clock::time_point now, std::chrono::milliseconds timeout) const {
    return now - lastSubmissionTime >= timeout;
  }
};
}  // namespace l4lb
