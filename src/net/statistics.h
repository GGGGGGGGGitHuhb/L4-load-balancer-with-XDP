#pragma once
#include <cstdint>

namespace l4lb::net {
/** 数据面事实；不携带日志文字，不依赖诊断限频或测试Observation。 */
enum class StatKind {
  Created,
  Closed,
  BytesC2b,
  BytesB2c,
  DatagramC2b,
  DatagramB2c,
  Rejected,
  Dropped,
  Error,
  Timeout
};

struct StatEvent {
  StatKind kind;
  std::uint64_t amount = 1;
};
}  // namespace l4lb::net
