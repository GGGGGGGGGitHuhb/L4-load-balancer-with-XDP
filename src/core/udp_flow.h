#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "config/config.h"
namespace l4lb {
/** 单 listener 的 UDP key；listener 端口和协议由实例隐含。 */
struct FlowKey {
  Endpoint client;
  std::array<std::uint8_t, 4> local;
  bool operator==(const FlowKey&) const = default;
};
struct FlowHash {
  std::size_t operator()(const FlowKey& key) const noexcept {
    std::size_t value = key.client.port;
    for (auto byte : key.client.address) value = value * 131 + byte;
    for (auto byte : key.local) value = value * 131 + byte;
    return value;
  }
};
/** UDP 成功发送零长包也算活动；收到或丢弃不更新。 */
struct UdpDeadline {
  using Clock = std::chrono::steady_clock;
  Clock::time_point last;
  void submitted(Clock::time_point now) { last = now; }
  bool expired(Clock::time_point now, std::chrono::milliseconds timeout) const {
    return now - last >= timeout;
  }
};
}  // namespace l4lb
