#include "metrics/metrics.h"

#include <unistd.h>

#include <limits>
#include <stdexcept>

namespace l4lb::metrics {
namespace {
void add(std::uint64_t& value, std::uint64_t n, bool& saturated) noexcept {
  auto max = std::numeric_limits<std::uint64_t>::max();
  if (n >= max - value) {
    value = max;
    saturated = true;
  } else
    value += n;
}

const char* health_name(Health state) {
  switch (state) {
    case Health::Disabled:
      return "disabled";
    case Health::Unknown:
      return "Unknown";
    case Health::Healthy:
      return "Healthy";
    case Health::Unhealthy:
      return "Unhealthy";
  }
  throw std::invalid_argument("invalid metrics health");
}
}  // namespace

Collector::Collector(Protocol protocol, std::size_t backend_count) {
  if (backend_count > 256 || backend_count == 0 ||
      (protocol != Protocol::kTcp && protocol != Protocol::kUdp))
    throw std::invalid_argument("invalid metrics collector");
  data_.protocol = protocol;
  data_.backend_count = backend_count;
}

bool Collector::update(net::StatEvent event) noexcept {
  using net::StatKind;
  auto increment = [&](std::uint64_t& value) {
    add(value, event.amount, data_.counter_saturated);
  };
  switch (event.kind) {
    case StatKind::Created:
      if (event.amount != 1 || data_.sessions_active >= 1024) return false;
      ++data_.sessions_active;
      increment(data_.sessions_created_total);
      break;
    case StatKind::Closed:
      if (event.amount != 1 || data_.sessions_active == 0) return false;
      --data_.sessions_active;
      increment(data_.sessions_closed_total);
      break;
    case StatKind::BytesC2b:
      increment(data_.bytes_c2b_total);
      break;
    case StatKind::BytesB2c:
      increment(data_.bytes_b2c_total);
      break;
    case StatKind::DatagramC2b:
      if (data_.protocol != Protocol::kUdp) return false;
      increment(data_.datagrams_c2b_total);
      break;
    case StatKind::DatagramB2c:
      if (data_.protocol != Protocol::kUdp) return false;
      increment(data_.datagrams_b2c_total);
      break;
    case StatKind::Rejected:
      increment(data_.rejected_total);
      break;
    case StatKind::Dropped:
      if (data_.protocol != Protocol::kUdp) return false;
      increment(data_.dropped_datagrams_total);
      break;
    case StatKind::Error:
      increment(data_.errors_total);
      break;
    case StatKind::Timeout:
      increment(data_.timeouts_total);
      break;
    default:
      return false;
  }
  return true;
}

std::string format(const Snapshot& s, std::string_view phase) {
  if (s.backend_count > 256 || (phase != "ready" && phase != "periodic" &&
                                phase != "final" && phase != "error"))
    throw std::invalid_argument("invalid metrics snapshot");
  std::string line;
  line.reserve(32768);
  line = "metrics {\"schema\":1,\"seq\":" + std::to_string(s.seq) +
         ",\"phase\":\"" + std::string(phase) + "\",\"protocol\":\"" +
         (s.protocol == Protocol::kTcp ? "tcp" : "udp") +
         "\",\"uptime_ms\":" + std::to_string(s.uptime_ms);
  auto field = [&](const char* key, std::uint64_t value) {
    line += ",\"";
    line += key;
    line += "\":";
    line += std::to_string(value);
  };
  field("sessions_created_total", s.sessions_created_total);
  field("sessions_closed_total", s.sessions_closed_total);
  field("sessions_active", s.sessions_active);
  field("bytes_c2b_total", s.bytes_c2b_total);
  field("bytes_b2c_total", s.bytes_b2c_total);
  field("datagrams_c2b_total", s.datagrams_c2b_total);
  field("datagrams_b2c_total", s.datagrams_b2c_total);
  field("rejected_total", s.rejected_total);
  field("dropped_datagrams_total", s.dropped_datagrams_total);
  field("errors_total", s.errors_total);
  field("timeouts_total", s.timeouts_total);
  line += s.counter_saturated ? ",\"counter_saturated\":true"
                              : ",\"counter_saturated\":false";
  line += ",\"backends\":[";
  for (std::size_t i = 0; i < s.backend_count; ++i) {
    if (i) line += ',';
    line += "{\"index\":" + std::to_string(i) + ",\"health\":\"" +
            health_name(s.backends[i].health) +
            "\",\"eligible\":" + (s.backends[i].eligible ? "true" : "false") +
            "}";
  }
  line += "]}\n";
  if (line.size() > 32768) throw std::length_error("metrics line too large");
  return line;
}

Output::Output(Collector& collector,
               std::function<void(std::span<Backend>)> backends,
               OutputOptions options)
    : collector_(collector),
      backends_(std::move(backends)),
      options_(std::move(options)),
      start_(now()) {}

Clock::time_point Output::now() const {
  return options_.now ? options_.now() : Clock::now();
}

void Output::emit(std::string_view phase) noexcept {
  if (!enabled_) return;
  try {
    auto snapshot = collector_.snapshot();
    bool saturated = false;
    add(seq_, 1, saturated);
    snapshot.seq = seq_;
    snapshot.counter_saturated |= saturated;
    snapshot.uptime_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now() - start_)
            .count();
    backends_(std::span(snapshot.backends.data(), snapshot.backend_count));
    auto line = options_.formatter ? options_.formatter(snapshot, phase)
                                   : format(snapshot, phase);
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

void Output::ready() noexcept {
  if (ready_ || ended_) return;
  ready_ = true;
  emit("ready");
  try {
    next_ = now() + std::chrono::seconds(1);
  } catch (...) {
    enabled_ = false;
  }
}

void Output::maintenance() noexcept {
  if (!ready_ || ended_ || !enabled_) return;
  try {
    if (now() < next_) return;
    emit("periodic");
    next_ = now() + std::chrono::seconds(1);
  } catch (...) {
    enabled_ = false;
  }
}

void Output::finish(bool error) noexcept {
  if (ended_) return;
  ended_ = true;
  emit(error ? "error" : "final");
}
}  // namespace l4lb::metrics
