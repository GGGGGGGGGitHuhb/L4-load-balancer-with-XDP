#include "metrics/Metrics.h"

#include <unistd.h>

#include <limits>
#include <stdexcept>

namespace l4lb::metrics {
namespace {
void addSaturatingCounter(std::uint64_t& value, std::uint64_t incrementAmount,
                          bool& saturated) noexcept {
  auto max = std::numeric_limits<std::uint64_t>::max();
  if (incrementAmount >= max - value) {
    value = max;
    saturated = true;
  } else
    value += incrementAmount;
}

const char* backendHealthName(BackendHealth state) {
  switch (state) {
    case BackendHealth::kDisabled:
      return "disabled";
    case BackendHealth::kUnknown:
      return "Unknown";
    case BackendHealth::kHealthy:
      return "Healthy";
    case BackendHealth::kUnhealthy:
      return "Unhealthy";
  }
  throw std::invalid_argument("invalid metrics health");
}

void appendCounterField(std::string& line, const char* key,
                        std::uint64_t value) {
  line += ",\"";
  line += key;
  line += "\":";
  line += std::to_string(value);
}
}  // namespace

MetricsCollector::MetricsCollector(Protocol protocol,
                                   std::size_t backendCount) {
  if (backendCount > 256 || backendCount == 0 ||
      (protocol != Protocol::kTcp && protocol != Protocol::kUdp))
    throw std::invalid_argument("invalid metrics collector");

  data_.protocol = protocol;
  data_.backendCount = backendCount;
}

void MetricsCollector::incrementCounter(
    std::uint64_t& value, std::uint64_t incrementAmount) noexcept {
  addSaturatingCounter(value, incrementAmount, data_.counterSaturated);
}

bool MetricsCollector::recordStatEvent(net::StatEvent event) noexcept {
  using net::StatKind;

  switch (event.kind) {
    case StatKind::kCreated:
      if (event.amount != 1 || data_.sessionsActive >= 1024) return false;
      ++data_.sessionsActive;
      incrementCounter(data_.sessionsCreatedTotal, event.amount);
      break;
    case StatKind::kClosed:
      if (event.amount != 1 || data_.sessionsActive == 0) return false;
      --data_.sessionsActive;
      incrementCounter(data_.sessionsClosedTotal, event.amount);
      break;
    case StatKind::kBytesC2b:
      incrementCounter(data_.bytesC2bTotal, event.amount);
      break;
    case StatKind::kBytesB2c:
      incrementCounter(data_.bytesB2cTotal, event.amount);
      break;
    case StatKind::kDatagramC2b:
      if (data_.protocol != Protocol::kUdp) return false;
      incrementCounter(data_.datagramsC2bTotal, event.amount);
      break;
    case StatKind::kDatagramB2c:
      if (data_.protocol != Protocol::kUdp) return false;
      incrementCounter(data_.datagramsB2cTotal, event.amount);
      break;
    case StatKind::kRejected:
      incrementCounter(data_.rejectedTotal, event.amount);
      break;
    case StatKind::kDropped:
      if (data_.protocol != Protocol::kUdp) return false;
      incrementCounter(data_.droppedDatagramsTotal, event.amount);
      break;
    case StatKind::kError:
      incrementCounter(data_.errorsTotal, event.amount);
      break;
    case StatKind::kTimeout:
      incrementCounter(data_.timeoutsTotal, event.amount);
      break;
    default:
      return false;
  }
  return true;
}

std::string formatMetricsSnapshot(const MetricsSnapshot& snapshot,
                                  std::string_view phase) {
  if (snapshot.backendCount > 256 || (phase != "ready" && phase != "periodic" &&
                                      phase != "final" && phase != "error"))
    throw std::invalid_argument("invalid metrics snapshot");

  std::string line;
  line.reserve(32768);
  line = "metrics {\"schema\":1,\"seq\":" + std::to_string(snapshot.seq) +
         ",\"phase\":\"" + std::string(phase) + "\",\"protocol\":\"" +
         (snapshot.protocol == Protocol::kTcp ? "tcp" : "udp") +
         "\",\"uptime_ms\":" + std::to_string(snapshot.uptimeMs);

  appendCounterField(line, "sessions_created_total",
                     snapshot.sessionsCreatedTotal);
  appendCounterField(line, "sessions_closed_total",
                     snapshot.sessionsClosedTotal);
  appendCounterField(line, "sessions_active", snapshot.sessionsActive);
  appendCounterField(line, "bytes_c2b_total", snapshot.bytesC2bTotal);
  appendCounterField(line, "bytes_b2c_total", snapshot.bytesB2cTotal);
  appendCounterField(line, "datagrams_c2b_total", snapshot.datagramsC2bTotal);
  appendCounterField(line, "datagrams_b2c_total", snapshot.datagramsB2cTotal);
  appendCounterField(line, "rejected_total", snapshot.rejectedTotal);
  appendCounterField(line, "dropped_datagrams_total",
                     snapshot.droppedDatagramsTotal);
  appendCounterField(line, "errors_total", snapshot.errorsTotal);
  appendCounterField(line, "timeouts_total", snapshot.timeoutsTotal);
  line += snapshot.counterSaturated ? ",\"counter_saturated\":true"
                                    : ",\"counter_saturated\":false";

  line += ",\"backends\":[";
  for (std::size_t backendIndex = 0; backendIndex < snapshot.backendCount;
       ++backendIndex) {
    if (backendIndex) line += ',';
    line += "{\"index\":" + std::to_string(backendIndex) + ",\"health\":\"" +
            backendHealthName(snapshot.backends[backendIndex].health) +
            "\",\"eligible\":" +
            (snapshot.backends[backendIndex].eligible ? "true" : "false") + "}";
  }

  line += "]}\n";
  if (line.size() > 32768) throw std::length_error("metrics line too large");
  return line;
}

MetricsOutput::MetricsOutput(MetricsCollector& collector,
                             BackendHealthProvider backends,
                             MetricsOutputOptions options)
    : collector_(collector),
      backendHealthProvider_(std::move(backends)),
      options_(std::move(options)),
      startTime_(currentTime()) {}

Clock::time_point MetricsOutput::currentTime() const {
  return options_.now ? options_.now() : Clock::now();
}

void MetricsOutput::emitSnapshot(std::string_view phase) noexcept {
  if (!enabled_) return;

  try {
    auto snapshot = collector_.snapshot();
    bool saturated = false;
    addSaturatingCounter(seq_, 1, saturated);
    snapshot.seq = seq_;
    snapshot.counterSaturated |= saturated;
    snapshot.uptimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            currentTime() - startTime_)
                            .count();
    backendHealthProvider_(
        std::span(snapshot.backends.data(), snapshot.backendCount));

    auto line = options_.formatter ? options_.formatter(snapshot, phase)
                                   : formatMetricsSnapshot(snapshot, phase);
    if (line.size() > 32768) {
      enabled_ = false;
      return;
    }

    auto result = options_.writer
                      ? options_.writer(line)
                      : ::write(STDERR_FILENO, line.data(), line.size());
    if (result < 0 || static_cast<std::size_t>(result) != line.size())
      enabled_ = false;
  } catch (...) {
    enabled_ = false;
  }
}

void MetricsOutput::emitReadySnapshot() noexcept {
  if (readyEmitted_ || finalized_) return;
  readyEmitted_ = true;
  emitSnapshot("ready");

  try {
    nextSnapshotTime_ = currentTime() + std::chrono::seconds(1);
  } catch (...) {
    enabled_ = false;
  }
}

void MetricsOutput::emitPeriodicSnapshotIfDue() noexcept {
  if (!readyEmitted_ || finalized_ || !enabled_) return;
  try {
    if (currentTime() < nextSnapshotTime_) return;
    emitSnapshot("periodic");
    nextSnapshotTime_ = currentTime() + std::chrono::seconds(1);
  } catch (...) {
    enabled_ = false;
  }
}

void MetricsOutput::emitFinalSnapshot(bool error) noexcept {
  if (finalized_) return;
  finalized_ = true;
  emitSnapshot(error ? "error" : "final");
}
}  // namespace l4lb::metrics
