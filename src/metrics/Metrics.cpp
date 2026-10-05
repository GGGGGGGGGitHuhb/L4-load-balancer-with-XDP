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
}  // namespace

MetricsCollector::MetricsCollector(Protocol protocol,
                                   std::size_t backendCount) {
  if (backendCount > 256 || backendCount == 0 ||
      (protocol != Protocol::kTcp && protocol != Protocol::kUdp))
    throw std::invalid_argument("invalid metrics collector");

  data_.protocol = protocol;
  data_.backendCount = backendCount;
}

bool MetricsCollector::recordStatEvent(net::StatEvent event) noexcept {
  using net::StatKind;
  auto increment = [&](std::uint64_t& value) {
    addSaturatingCounter(value, event.amount, data_.counterSaturated);
  };

  switch (event.kind) {
    case StatKind::kCreated:
      if (event.amount != 1 || data_.sessionsActive >= 1024) return false;
      ++data_.sessionsActive;
      increment(data_.sessionsCreatedTotal);
      break;
    case StatKind::kClosed:
      if (event.amount != 1 || data_.sessionsActive == 0) return false;
      --data_.sessionsActive;
      increment(data_.sessionsClosedTotal);
      break;
    case StatKind::kBytesC2b:
      increment(data_.bytesC2bTotal);
      break;
    case StatKind::kBytesB2c:
      increment(data_.bytesB2cTotal);
      break;
    case StatKind::kDatagramC2b:
      if (data_.protocol != Protocol::kUdp) return false;
      increment(data_.datagramsC2bTotal);
      break;
    case StatKind::kDatagramB2c:
      if (data_.protocol != Protocol::kUdp) return false;
      increment(data_.datagramsB2cTotal);
      break;
    case StatKind::kRejected:
      increment(data_.rejectedTotal);
      break;
    case StatKind::kDropped:
      if (data_.protocol != Protocol::kUdp) return false;
      increment(data_.droppedDatagramsTotal);
      break;
    case StatKind::kError:
      increment(data_.errorsTotal);
      break;
    case StatKind::kTimeout:
      increment(data_.timeoutsTotal);
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

  auto field = [&](const char* key, std::uint64_t value) {
    line += ",\"";
    line += key;
    line += "\":";
    line += std::to_string(value);
  };

  field("sessions_created_total", snapshot.sessionsCreatedTotal);
  field("sessions_closed_total", snapshot.sessionsClosedTotal);
  field("sessions_active", snapshot.sessionsActive);
  field("bytes_c2b_total", snapshot.bytesC2bTotal);
  field("bytes_b2c_total", snapshot.bytesB2cTotal);
  field("datagrams_c2b_total", snapshot.datagramsC2bTotal);
  field("datagrams_b2c_total", snapshot.datagramsB2cTotal);
  field("rejected_total", snapshot.rejectedTotal);
  field("dropped_datagrams_total", snapshot.droppedDatagramsTotal);
  field("errors_total", snapshot.errorsTotal);
  field("timeouts_total", snapshot.timeoutsTotal);
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

MetricsOutput::MetricsOutput(
    MetricsCollector& collector,
    std::function<void(std::span<BackendHealthSnapshot>)> backends,
    MetricsOutputOptions options)
    : collector_(collector),
      copyBackendHealth_(std::move(backends)),
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
    copyBackendHealth_(
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
