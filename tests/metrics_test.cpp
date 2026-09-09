#include "metrics/metrics.h"

#include <fcntl.h>
#include <unistd.h>

#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#include "control/metrics_service.h"

namespace l4lb::metrics {
struct TestAccess {
  static void seed(Collector& c, Snapshot s) { c.data_ = s; }

  static void sequence(Output& output, std::uint64_t n) { output.seq_ = n; }
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
        auto result = parse_config(first ? field + base : base + field);
        auto* c = std::get_if<Config>(&result);
        check(c && c->metrics == (std::string(metric) == "off"
                                      ? MetricsKind::kOff
                                      : MetricsKind::kStderr),
              "config combinations/order");
      }
  for (auto bad : {"metrics=", "metrics=STDERR", "metrics=OFF", "Metrics=off",
                   "metrics=unknown\nhealth_check=bad"}) {
    auto result = parse_config(std::string(bad) + "\n" + base);
    auto* e = std::get_if<ConfigError>(&result);
    check(e && e->line == 1, "config first error");
  }
  for (auto second : {"off", "stderr", "bad"}) {
    auto result = parse_config(std::string("metrics=off\nmetrics=") + second +
                               "\n" + base);
    auto* e = std::get_if<ConfigError>(&result);
    check(e && e->line == 2, "config duplicate");
  }
  auto result = parse_config(base);
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
  Collector c(Protocol::kUdp, 2), other(Protocol::kTcp, 1);
  auto zero = c.snapshot();
  check(zero.sessions_active == 0 && zero.errors_total == 0 &&
            !zero.counter_saturated,
        "initial");
  check(!c.update({StatKind::Closed}) && c.snapshot().sessions_active == 0,
        "duplicate close detected");
  check(c.update({StatKind::Created}) && c.update({StatKind::Created}),
        "created");
  for (auto event : {net::StatEvent{StatKind::BytesC2b, 7},
                     {StatKind::BytesC2b, 3},
                     {StatKind::BytesB2c, 4},
                     {StatKind::DatagramC2b},
                     {StatKind::DatagramC2b},
                     {StatKind::DatagramB2c},
                     {StatKind::Rejected},
                     {StatKind::Dropped},
                     {StatKind::Error, 3},
                     {StatKind::Timeout, 2}})
    check(c.update(event), "event accepted");
  auto s = c.snapshot();
  check(s.sessions_created_total == 2 && s.sessions_active == 2 &&
            s.bytes_c2b_total == 10 && s.bytes_b2c_total == 4 &&
            s.datagrams_c2b_total == 2 && s.datagrams_b2c_total == 1 &&
            s.rejected_total == 1 && s.dropped_datagrams_total == 1 &&
            s.errors_total == 3 && s.timeouts_total == 2,
        "every counter");
  check(c.update({StatKind::Closed}) && c.update({StatKind::Closed}), "close");
  check(c.snapshot().sessions_closed_total == 2 &&
            c.snapshot().sessions_active == 0 &&
            c.snapshot().bytes_c2b_total == 10,
        "close does not duplicate bytes");
  check(other.snapshot().bytes_c2b_total == 0 &&
            !other.update({StatKind::DatagramC2b}),
        "isolation/TCP packets zero");
  auto max = std::numeric_limits<std::uint64_t>::max();
  for (auto member :
       {&Snapshot::sessions_created_total, &Snapshot::sessions_closed_total,
        &Snapshot::bytes_c2b_total, &Snapshot::bytes_b2c_total,
        &Snapshot::datagrams_c2b_total, &Snapshot::datagrams_b2c_total,
        &Snapshot::rejected_total, &Snapshot::dropped_datagrams_total,
        &Snapshot::errors_total, &Snapshot::timeouts_total}) {
    Snapshot seeded = zero;
    seeded.*member = max - 1;
    seeded.sessions_active = 1;
    TestAccess::seed(c, seeded);
    for (auto kind : {StatKind::Created, StatKind::Closed, StatKind::BytesC2b,
                      StatKind::BytesB2c, StatKind::DatagramC2b,
                      StatKind::DatagramB2c, StatKind::Rejected,
                      StatKind::Dropped, StatKind::Error, StatKind::Timeout})
      check(c.update({kind}), "saturating event");
    check(c.snapshot().*member == max && c.snapshot().counter_saturated,
          "exact saturation");
  }
  Snapshot seeded = zero;
  seeded.sessions_active = 1024;
  TestAccess::seed(c, seeded);
  check(!c.update({StatKind::Created}) && c.snapshot().sessions_active == 1024,
        "active cap invariant");
  TestAccess::seed(c, zero);
  c.update({StatKind::BytesC2b, max});
  c.update({StatKind::BytesC2b, max});
  check(c.snapshot().bytes_c2b_total == max, "no wrap");
  Collector restart(Protocol::kUdp, 1);
  check(restart.snapshot().bytes_c2b_total == 0, "restart zero");
  s = c.snapshot();
  for (auto member :
       {&Snapshot::sessions_created_total, &Snapshot::sessions_closed_total,
        &Snapshot::sessions_active, &Snapshot::bytes_c2b_total,
        &Snapshot::bytes_b2c_total, &Snapshot::datagrams_c2b_total,
        &Snapshot::datagrams_b2c_total, &Snapshot::rejected_total,
        &Snapshot::dropped_datagrams_total, &Snapshot::errors_total,
        &Snapshot::timeouts_total})
    s.*member = max;
  s.backend_count = 256;
  s.seq = max;
  s.uptime_ms = max;
  for (auto& b : s.backends) b = {Health::Unhealthy, false};
  auto line = format(s, "periodic");
  check(line.size() <= 32768 && line.ends_with("}\n"), "maximum line");
  std::cout << "max-line-bytes=" << line.size() << '\n';
}

void output() {
  Collector c(Protocol::kTcp, 1);
  Clock::time_point time{};
  std::vector<std::string> lines;
  OutputOptions opt;
  opt.now = [&] { return time; };
  opt.writer = [&](std::string_view line) {
    lines.emplace_back(line);
    return std::ptrdiff_t(line.size());
  };
  auto backend = [](std::span<Backend> values) {
    values[0] = {Health::Unknown, false};
  };
  Output out(c, backend, opt);
  out.maintenance();
  check(lines.empty(), "no periodic before ready");
  out.ready();
  out.ready();
  check(lines.size() == 1 &&
            lines[0].find("\"phase\":\"ready\"") != std::string::npos,
        "ready exactly once");
  time += 999ms;
  out.maintenance();
  check(lines.size() == 1, "before deadline");
  time += 1ms;
  out.maintenance();
  check(lines.size() == 2, "exact deadline");
  out.maintenance();
  check(lines.size() == 2, "same clock idempotent");
  time += 30s;
  out.maintenance();
  check(lines.size() == 3, "no catchup");
  TestAccess::sequence(out, std::numeric_limits<std::uint64_t>::max() - 1);
  time += 1s;
  out.maintenance();
  check(lines.back().find("\"counter_saturated\":true") != std::string::npos,
        "seq saturation");
  out.finish(false);
  out.finish(true);
  out.maintenance();
  check(lines.size() == 5 &&
            lines.back().find("\"phase\":\"final\"") != std::string::npos,
        "one final");
  for (int mode = 0; mode < 3; ++mode) {
    int attempts = 0;
    OutputOptions failure = opt;
    failure.writer = [&](std::string_view line) {
      ++attempts;
      return mode == 0 ? -1 : std::ptrdiff_t(line.size() - 1);
    };
    if (mode == 2)
      failure.formatter = [](const Snapshot&, std::string_view) -> std::string {
        throw std::bad_alloc();
      };
    Output failing(c, backend, failure);
    failing.ready();
    failing.maintenance();
    failing.finish(true);
    check(!failing.enabled() && attempts == (mode == 2 ? 0 : 1),
          "failure disables no retries");
  }
  int fd = open("/dev/full", O_WRONLY | O_CLOEXEC);
  check(fd >= 0, "dev full opened");
  int attempts = 0;
  OutputOptions full = opt;
  full.writer = [&](std::string_view line) {
    ++attempts;
    return write(fd, line.data(), line.size());
  };
  Output failing(c, backend, full);
  failing.ready();
  failing.finish(true);
  close(fd);
  check(!failing.enabled() && attempts == 1, "real dev full disables");
  OutputOptions exception = opt;
  exception.writer = [](std::string_view) -> std::ptrdiff_t {
    throw std::runtime_error("sink");
  };
  Output error(c, backend, exception);
  bool original = false;
  try {
    throw std::runtime_error("service");
  } catch (...) {
    error.finish(true);
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
