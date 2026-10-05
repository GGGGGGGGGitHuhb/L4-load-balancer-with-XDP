#pragma once
#include <cstdint>

namespace l4lb::net {
/** 数据面事实；不携带日志文字，不依赖诊断限频或测试Observation。 */
enum class StatKind {
  kCreated,
  kClosed,
  kBytesC2b,
  kBytesB2c,
  kDatagramC2b,
  kDatagramB2c,
  kRejected,
  kDropped,
  kError,
  kTimeout
};

struct StatEvent {
  StatKind kind;
  std::uint64_t amount = 1;
};
}  // namespace l4lb::net
