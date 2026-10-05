#include "metrics/Metrics.h"

#include <fcntl.h>
#include <unistd.h>

#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#include "control/MetricsService.h"

namespace l4lb::metrics {
struct TestAccess {
  static void seed(MetricsCollector& c, MetricsSnapshot s) { c.data_ = s; }

  static void sequence(MetricsOutput& output, std::uint64_t n) {
    output.seq_ = n;
  }
};
}  // namespace l4lb::metrics

using namespace l4lb;
using namespace l4lb::metrics;
using net::StatKind;
using namespace std::chrono_literals;

namespace {
int checks = 0;

void check(bool ok, const char* reason) {
  ++checks;
  if (!ok) throw std::runtime_error(reason);
}

void config() {
  const std::string base = "listen=127.0.0.1:1234\nbackend=127.0.0.1:1235\n";
  for (auto health : {"off", "tcp_connect"})
    for (auto metric : {"off", "stderr"})
      for (bool first : {true, false}) {
        auto field = std::string("metrics=") + metric +
                     "\nhealth_check=" + health + "\n";
        auto result = parseConfig(first ? field + base : base + field);
        auto* c = std::get_if<Config>(&result);
        check(c && c->metrics == (std::string(metric) == "off"
                                      ? MetricsKind::kOff
                                      : MetricsKind::kStderr),
              "config combinations/order");
      }
  for (auto bad : {"metrics=", "metrics=STDERR", "metrics=OFF", "Metrics=off",
                   "metrics=unknown\nhealth_check=bad"}) {
    auto result = parseConfig(std::string(bad) + "\n" + base);
    auto* e = std::get_if<ConfigError>(&result);
    check(e && e->line == 1, "config first error");
  }
  for (auto second : {"off", "stderr", "bad"}) {
    auto result = parseConfig(std::string("metrics=off\nmetrics=") + second +
                              "\n" + base);
    auto* e = std::get_if<ConfigError>(&result);
    check(e && e->line == 2, "config duplicate");
  }
  auto result = parseConfig(base);
  check(std::get<Config>(result).metrics == MetricsKind::kOff, "default off");
  Config c = std::get<Config>(result);
  std::unique_ptr<HealthSelection> selection;
  MetricsService off(c, selection);
  check(!off.enabled(), "off no collector/output");
  c.metrics = static_cast<MetricsKind>(77);
  bool threw = false;
  try {
    MetricsService invalid(c, selection);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  check(threw, "unknown internal metrics enum");
}

void model() {
  MetricsCollector c(Protocol::kUdp, 2), other(Protocol::kTcp, 1);
  auto zero = c.snapshot();
  check(zero.sessionsActive == 0 && zero.errorsTotal == 0 &&
            !zero.counterSaturated,
        "initial");
  check(!c.recordStatEvent({StatKind::kClosed}) &&
            c.snapshot().sessionsActive == 0,
        "duplicate close detected");
  check(c.recordStatEvent({StatKind::kCreated}) &&
            c.recordStatEvent({StatKind::kCreated}),
        "created");
  for (auto event : {net::StatEvent{StatKind::kBytesC2b, 7},
                     {StatKind::kBytesC2b, 3},
                     {StatKind::kBytesB2c, 4},
                     {StatKind::kDatagramC2b},
                     {StatKind::kDatagramC2b},
                     {StatKind::kDatagramB2c},
                     {StatKind::kRejected},
                     {StatKind::kDropped},
                     {StatKind::kError, 3},
                     {StatKind::kTimeout, 2}})
    check(c.recordStatEvent(event), "event accepted");
  auto s = c.snapshot();
  check(s.sessionsCreatedTotal == 2 && s.sessionsActive == 2 &&
            s.bytesC2bTotal == 10 && s.bytesB2cTotal == 4 &&
            s.datagramsC2bTotal == 2 && s.datagramsB2cTotal == 1 &&
            s.rejectedTotal == 1 && s.droppedDatagramsTotal == 1 &&
            s.errorsTotal == 3 && s.timeoutsTotal == 2,
        "every counter");
  check(c.recordStatEvent({StatKind::kClosed}) &&
            c.recordStatEvent({StatKind::kClosed}),
        "close");
  check(c.snapshot().sessionsClosedTotal == 2 &&
            c.snapshot().sessionsActive == 0 &&
            c.snapshot().bytesC2bTotal == 10,
        "close does not duplicate bytes");
  check(other.snapshot().bytesC2bTotal == 0 &&
            !other.recordStatEvent({StatKind::kDatagramC2b}),
        "isolation/TCP packets zero");
  auto max = std::numeric_limits<std::uint64_t>::max();
  for (auto member :
       {&MetricsSnapshot::sessionsCreatedTotal,
        &MetricsSnapshot::sessionsClosedTotal, &MetricsSnapshot::bytesC2bTotal,
        &MetricsSnapshot::bytesB2cTotal, &MetricsSnapshot::datagramsC2bTotal,
        &MetricsSnapshot::datagramsB2cTotal, &MetricsSnapshot::rejectedTotal,
        &MetricsSnapshot::droppedDatagramsTotal, &MetricsSnapshot::errorsTotal,
        &MetricsSnapshot::timeoutsTotal}) {
    MetricsSnapshot seeded = zero;
    seeded.*member = max - 1;
    seeded.sessionsActive = 1;
    TestAccess::seed(c, seeded);
    for (auto kind :
         {StatKind::kCreated, StatKind::kClosed, StatKind::kBytesC2b,
          StatKind::kBytesB2c, StatKind::kDatagramC2b, StatKind::kDatagramB2c,
          StatKind::kRejected, StatKind::kDropped, StatKind::kError,
          StatKind::kTimeout})
      check(c.recordStatEvent({kind}), "saturating event");
    check(c.snapshot().*member == max && c.snapshot().counterSaturated,
          "exact saturation");
  }
  MetricsSnapshot seeded = zero;
  seeded.sessionsActive = 1024;
  TestAccess::seed(c, seeded);
  check(!c.recordStatEvent({StatKind::kCreated}) &&
            c.snapshot().sessionsActive == 1024,
        "active cap invariant");
  TestAccess::seed(c, zero);
  c.recordStatEvent({StatKind::kBytesC2b, max});
  c.recordStatEvent({StatKind::kBytesC2b, max});
  check(c.snapshot().bytesC2bTotal == max, "no wrap");
  MetricsCollector restart(Protocol::kUdp, 1);
  check(restart.snapshot().bytesC2bTotal == 0, "restart zero");
  s = c.snapshot();
  for (auto member :
       {&MetricsSnapshot::sessionsCreatedTotal,
        &MetricsSnapshot::sessionsClosedTotal, &MetricsSnapshot::sessionsActive,
        &MetricsSnapshot::bytesC2bTotal, &MetricsSnapshot::bytesB2cTotal,
        &MetricsSnapshot::datagramsC2bTotal,
        &MetricsSnapshot::datagramsB2cTotal, &MetricsSnapshot::rejectedTotal,
        &MetricsSnapshot::droppedDatagramsTotal, &MetricsSnapshot::errorsTotal,
        &MetricsSnapshot::timeoutsTotal})
    s.*member = max;
  s.backendCount = 256;
  s.seq = max;
  s.uptimeMs = max;
  for (auto& b : s.backends) b = {BackendHealth::kUnhealthy, false};
  auto line = formatMetricsSnapshot(s, "periodic");
  check(line.size() <= 32768 && line.ends_with("}\n"), "maximum line");
  std::cout << "max-line-bytes=" << line.size() << '\n';
}

void output() {
  MetricsCollector c(Protocol::kTcp, 1);
  Clock::time_point time{};
  std::vector<std::string> lines;
  MetricsOutputOptions opt;
  opt.now = [&] { return time; };
  opt.writer = [&](std::string_view line) {
    lines.emplace_back(line);
    return std::ptrdiff_t(line.size());
  };
  auto backend = [](std::span<BackendHealthSnapshot> values) {
    values[0] = {BackendHealth::kUnknown, false};
  };
  MetricsOutput out(c, backend, opt);
  out.emitPeriodicSnapshotIfDue();
  check(lines.empty(), "no periodic before ready");
  out.emitReadySnapshot();
  out.emitReadySnapshot();
  check(lines.size() == 1 &&
            lines[0].find("\"phase\":\"ready\"") != std::string::npos,
        "ready exactly once");
  time += 999ms;
  out.emitPeriodicSnapshotIfDue();
  check(lines.size() == 1, "before deadline");
  time += 1ms;
  out.emitPeriodicSnapshotIfDue();
  check(lines.size() == 2, "exact deadline");
  out.emitPeriodicSnapshotIfDue();
  check(lines.size() == 2, "same clock idempotent");
  time += 30s;
  out.emitPeriodicSnapshotIfDue();
  check(lines.size() == 3, "no catchup");
  TestAccess::sequence(out, std::numeric_limits<std::uint64_t>::max() - 1);
  time += 1s;
  out.emitPeriodicSnapshotIfDue();
  check(lines.back().find("\"counter_saturated\":true") != std::string::npos,
        "seq saturation");
  out.emitFinalSnapshot(false);
  out.emitFinalSnapshot(true);
  out.emitPeriodicSnapshotIfDue();
  check(lines.size() == 5 &&
            lines.back().find("\"phase\":\"final\"") != std::string::npos,
        "one final");
  for (int mode = 0; mode < 3; ++mode) {
    int attempts = 0;
    MetricsOutputOptions failure = opt;
    failure.writer = [&](std::string_view line) {
      ++attempts;
      return mode == 0 ? -1 : std::ptrdiff_t(line.size() - 1);
    };
    if (mode == 2)
      failure.formatter = [](const MetricsSnapshot&,
                             std::string_view) -> std::string {
        throw std::bad_alloc();
      };
    MetricsOutput failing(c, backend, failure);
    failing.emitReadySnapshot();
    failing.emitPeriodicSnapshotIfDue();
    failing.emitFinalSnapshot(true);
    check(!failing.enabled() && attempts == (mode == 2 ? 0 : 1),
          "failure disables no retries");
  }
  int fd = open("/dev/full", O_WRONLY | O_CLOEXEC);
  check(fd >= 0, "dev full opened");
  int attempts = 0;
  MetricsOutputOptions full = opt;
  full.writer = [&](std::string_view line) {
    ++attempts;
    return write(fd, line.data(), line.size());
  };
  MetricsOutput failing(c, backend, full);
  failing.emitReadySnapshot();
  failing.emitFinalSnapshot(true);
  close(fd);
  check(!failing.enabled() && attempts == 1, "real dev full disables");
  MetricsOutputOptions exception = opt;
  exception.writer = [](std::string_view) -> std::ptrdiff_t {
    throw std::runtime_error("sink");
  };
  MetricsOutput error(c, backend, exception);
  bool original = false;
  try {
    throw std::runtime_error("service");
  } catch (...) {
    error.emitFinalSnapshot(true);
    try {
      throw;
    } catch (const std::runtime_error& e) {
      original = std::string(e.what()) == "service";
    }
  }
  check(original, "original exception preserved");
}
}  // namespace

int main() {
  try {
    config();
    model();
    output();
    std::cout << "PASS metrics checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL metrics " << e.what() << '\n';
    return 1;
  }
}
