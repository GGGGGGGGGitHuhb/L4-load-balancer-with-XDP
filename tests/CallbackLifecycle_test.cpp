#include <fcntl.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "../src/net/TcpReactor.cpp"
#include "../src/net/UdpReactor.cpp"
#include "control/MetricsService.h"
#include "health/TcpHealthChecker.h"
#include "metrics/Metrics.h"

namespace l4lb::net {
namespace {
void requireLifecycle(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::size_t countOpenFds() {
  return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                       std::filesystem::directory_iterator{});
}

struct FirstReceiverFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct LaterReceiverFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct ReactorTestAccess {
  TcpReactor* reactor = nullptr;
  metrics::MetricsCollector collector{Protocol::kTcp, 1};
  std::vector<std::string> sequence;
  std::vector<int> sessionFds;
  int readyCalls = 0;
  bool detachFailure = false;

  void requireDetached() const {
    requireLifecycle(reactor->sessions_.empty() && reactor->tokens_.size() == 0,
                     "TCP identity withdrawn before receiver");
    for (int fd : sessionFds)
      requireLifecycle(fcntl(fd, F_GETFD) < 0,
                       "TCP socket closed before receiver");
  }

  void onReady() { ++readyCalls; }

  void onReplacementReady() { readyCalls += 10; }

  void onStatistics(StatEvent event) {
    requireDetached();
    requireLifecycle(collector.recordStatEvent(event), "TCP gauge transition");
    if (event.kind == StatKind::kError) {
      sequence.push_back("error-stat");
      throw FirstReceiverFailure("error-stat");
    }
    sequence.push_back("closed-stat");
    throw LaterReceiverFailure("closed-stat");
  }

  void onDiagnostic(const std::string&, int) {
    requireDetached();
    sequence.push_back("diagnostic");
    throw LaterReceiverFailure("diagnostic");
  }

  void onSession(const SessionEvent& event) {
    requireDetached();
    requireLifecycle(
        !event.accepted && event.id == 7 && event.reason == "fixture-close",
        "TCP lifecycle owner remains valid after erase");
    sequence.push_back("session");
    if (!detachFailure) throw FirstReceiverFailure("session");
    throw LaterReceiverFailure("session");
  }

  void onObservation(const Observation& event) {
    if (event.kind != "closed") return;
    requireDetached();
    sequence.push_back("observation");
    throw LaterReceiverFailure("observation");
  }

  void addSession(TcpReactor& current, std::array<Fd, 2>& peers) {
    auto owner = std::make_unique<Session>();
    owner->id = 7;
    owner->connecting = false;

    for (int side = 0; side < 2; ++side) {
      int pair[2];
      requireLifecycle(
          socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                     pair) == 0,
          "TCP fixture socketpair");
      peers[side] = Fd(pair[1]);
      owner->ends[side].fd = Fd(pair[0]);
      owner->ends[side].token = current.tokens_.registerEndpoint(7, side);
      sessionFds.push_back(pair[0]);
    }

    auto& session = *owner;
    current.sessions_.emplace(7, std::move(owner));
    current.updateSessionInterests(session);

    requireLifecycle(collector.recordStatEvent({StatKind::kCreated}),
                     "TCP created gauge");
  }

  void exercise(bool failDetach) {
    auto fdBaseline = countOpenFds();
    detachFailure = failDetach;

    TcpReactorCallbacks callbacks;
    TcpReactorOptions options;
    callbacks.setReadyCallback([this] { onReady(); });
    requireLifecycle(readyCalls == 0, "TCP setter does not execute receiver");
    callbacks.setReadyCallback([this] { onReplacementReady(); });
    callbacks.setReadyCallback({});
    requireLifecycle(
        !callbacks.readyCallback() && readyCalls == 0,
        "TCP ready replacement and clear preserve nullable semantics");

    callbacks.setReadyCallback([this] { onReady(); });
    callbacks.setStatisticsCallback(
        [this](StatEvent event) { onStatistics(event); });
    callbacks.setSessionCallback(
        [this](const SessionEvent& event) { onSession(event); });
    callbacks.setDiagnosticCallback(
        [this](const std::string& operation, int error) {
          onDiagnostic(operation, error);
        });
    options.setObservationCallback(
        [this](const Observation& event) { onObservation(event); });

    {
      std::array<Fd, 2> peers;
      TcpReactor current({{127, 0, 0, 1}, 0}, callbacks, options);
      reactor = &current;

      current.stopRequested_ = true;
      requireLifecycle(current.runTcpEventLoop() == 0 && readyCalls == 1,
                       "TCP receiver bound before actual ready event");

      addSession(current, peers);
      if (detachFailure)
        current.epollFd_ = Fd(open("/dev/null", O_RDONLY | O_CLOEXEC));

      bool firstCaught = false;
      try {
        current.closeSession(7, "fixture-close", 0);
      } catch (const FirstReceiverFailure& failure) {
        firstCaught = true;
        requireLifecycle(
            std::string(failure.what()) ==
                (detachFailure ? "error-stat" : "session"),
            "TCP exact first exception survives multiple failures");
      }
      requireLifecycle(firstCaught, "TCP first exception type preserved");

      auto notificationCount = sequence.size();
      current.closeSession(7, "duplicate-close", 0);
      requireLifecycle(sequence.size() == notificationCount,
                       "TCP duplicate close is silent");

      requireLifecycle(collector.snapshot().sessionsActive == 0 &&
                           collector.snapshot().sessionsClosedTotal == 1,
                       "TCP closed gauge exactly once despite failures");
    }

    requireLifecycle(countOpenFds() == fdBaseline,
                     "TCP all fixture fds released");

    const std::vector<std::string> expected =
        detachFailure
            ? std::vector<std::string>{"error-stat", "diagnostic",
                                       "error-stat", "diagnostic",
                                       "session",    "closed-stat",
                                       "observation"}
            : std::vector<std::string>{"session", "closed-stat", "observation"};
    requireLifecycle(sequence == expected,
                     "TCP complete reporting order and silent destructor");

    std::cout << "TCP multi-failure detach=" << detachFailure << " sequence=";
    for (const auto& entry : sequence) std::cout << entry << ',';
    std::cout << " first=" << (detachFailure ? "error-stat" : "session")
              << '\n';
  }
};

struct UdpTestAccess {
  UdpReactor* reactor = nullptr;
  metrics::MetricsCollector collector{Protocol::kUdp, 1};
  std::vector<std::string> sequence;
  int flowFd = -1;
  bool detachFailure = false;
  int readyCalls = 0;

  void requireDetached() const {
    requireLifecycle(
        reactor->flows_.empty() && reactor->flowTokensByKey_.empty(),
        "UDP both indices withdrawn before receiver");
    requireLifecycle(fcntl(flowFd, F_GETFD) < 0,
                     "UDP fd closed before receiver");
  }

  void onReady() { ++readyCalls; }

  void onStatistics(StatEvent event) {
    requireDetached();
    requireLifecycle(collector.recordStatEvent(event), "UDP gauge transition");
    if (event.kind == StatKind::kError) {
      sequence.push_back("error-stat");
      throw FirstReceiverFailure("error-stat");
    }
    sequence.push_back("closed-stat");
    if (!detachFailure) throw FirstReceiverFailure("closed-stat");
    throw LaterReceiverFailure("closed-stat");
  }

  void onDiagnostic(const std::string&, int) {
    requireDetached();
    sequence.push_back("diagnostic");
    throw LaterReceiverFailure("diagnostic");
  }

  void onObservation(const UdpObservation& event) {
    if (event.kind != "fixture-close") return;
    requireDetached();
    sequence.push_back("observation");
    throw LaterReceiverFailure("observation");
  }

  void onFlow(const UdpFlowEvent& event) {
    requireDetached();
    requireLifecycle(event.id == 9 && event.reason == "fixture-close",
                     "UDP owner remains valid through final receiver");
    sequence.push_back("flow");
    throw LaterReceiverFailure("flow");
  }

  void exercise(bool failDetach) {
    auto fdBaseline = countOpenFds();
    detachFailure = failDetach;

    UdpReactorCallbacks callbacks;
    UdpReactorOptions options;
    callbacks.setReadyCallback([this] { onReady(); });
    requireLifecycle(readyCalls == 0, "UDP setter does not execute receiver");
    callbacks.setReadyCallback({});
    requireLifecycle(!callbacks.readyCallback(),
                     "UDP ready remains optional after clear");

    callbacks.setReadyCallback([this] { onReady(); });
    callbacks.setStatisticsCallback(
        [this](StatEvent event) { onStatistics(event); });
    callbacks.setDiagnosticCallback(
        [this](const std::string& operation, int error) {
          onDiagnostic(operation, error);
        });
    callbacks.setFlowCallback(
        [this](const UdpFlowEvent& event) { onFlow(event); });
    options.setObservationCallback(
        [this](const UdpObservation& event) { onObservation(event); });

    {
      UdpReactor current({{127, 0, 0, 1}, 0}, callbacks, options);
      reactor = &current;

      callbacks.readyCallback()();
      requireLifecycle(readyCalls == 1,
                       "UDP bound receiver executes after registration");

      auto flow = std::make_unique<UdpFlow>();
      flow->token = 9;
      flow->backend = Endpoint{{127, 0, 0, 1}, 1};
      flow->fd =
          Fd(socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
      flow->deadline = UdpIdleDeadline{UdpIdleDeadline::Clock::now()};
      flowFd = flow->fd.fd();
      requireLifecycle(flowFd >= 0, "UDP fixture flow socket");

      current.flowTokensByKey_.emplace(flow->key, 9);
      current.registerFlowSocket(flowFd, 9);
      current.flows_.emplace(9, std::move(flow));
      requireLifecycle(collector.recordStatEvent({StatKind::kCreated}),
                       "UDP created gauge");

      if (detachFailure)
        current.epollFd_ = Fd(open("/dev/null", O_RDONLY | O_CLOEXEC));

      bool firstCaught = false;
      try {
        current.closeFlow(9, "fixture-close");
      } catch (const FirstReceiverFailure& failure) {
        firstCaught = true;
        requireLifecycle(
            std::string(failure.what()) ==
                (detachFailure ? "error-stat" : "closed-stat"),
            "UDP exact first exception survives multiple failures");
      }
      requireLifecycle(firstCaught, "UDP first exception type preserved");

      auto notificationCount = sequence.size();
      current.closeFlow(9, "duplicate-close");
      requireLifecycle(sequence.size() == notificationCount,
                       "UDP duplicate close is silent");

      requireLifecycle(collector.snapshot().sessionsActive == 0 &&
                           collector.snapshot().sessionsClosedTotal == 1,
                       "UDP closed gauge exactly once despite failures");
    }

    requireLifecycle(countOpenFds() == fdBaseline,
                     "UDP all fixture fds released");

    const std::vector<std::string> expected =
        detachFailure
            ? std::vector<std::string>{"error-stat", "diagnostic",
                                       "closed-stat", "observation", "flow"}
            : std::vector<std::string>{"closed-stat", "observation", "flow"};
    requireLifecycle(sequence == expected,
                     "UDP complete reporting order and silent destructor");

    std::cout << "UDP multi-failure detach=" << detachFailure << " sequence=";
    for (const auto& entry : sequence) std::cout << entry << ',';
    std::cout << " first=" << (detachFailure ? "error-stat" : "closed-stat")
              << '\n';
  }
};

class HealthReceiverFixture {
 public:
  health::Clock::time_point time{};
  health::TcpHealthChecker* checker = nullptr;
  int notifications = 0;
  std::size_t idleFdCount = 0;

  health::Clock::time_point currentTime() const { return time; }

  int connectImmediately(int, const sockaddr*, socklen_t) { return 0; }

  int connectPending(int, const sockaddr*, socklen_t) {
    errno = EINPROGRESS;
    return -1;
  }

  int noReadyEvents(int, epoll_event*, int) { return 0; }

  void onHealthChange(const health::HealthChange& change) {
    ++notifications;
    requireLifecycle(
        change.to == health::HealthStatus::kHealthy &&
            checker->backendState(0).status == change.to &&
            checker->activeProbeCount() == 0,
        "HealthChange observes committed state and withdrawn token");
    requireLifecycle(countOpenFds() == idleFdCount,
                     "health socket closed before event");
  }

  void exercise() {
    auto fdBaseline = countOpenFds();
    health::TcpHealthCheckOptions options;
    options.now = [this] { return currentTime(); };
    options.connectCall = [this](int fd, const sockaddr* address,
                                 socklen_t length) {
      return connectImmediately(fd, address, length);
    };
    options.epollPollCall = [this](int fd, epoll_event* events, int count) {
      return noReadyEvents(fd, events, count);
    };

    {
      health::TcpHealthChecker current({Endpoint{{127, 0, 0, 1}, 1}}, options);
      checker = &current;
      idleFdCount = countOpenFds();

      current.setHealthChangeCallback(
          [this](const health::HealthChange& change) {
            onHealthChange(change);
          });
      requireLifecycle(notifications == 0, "health setter has no event");

      current.pollHealthProbes();
      requireLifecycle(notifications == 0,
                       "health threshold before second success");

      time += std::chrono::seconds(1);
      current.pollHealthProbes();
      requireLifecycle(notifications == 1,
                       "health bound before first threshold event");
    }

    options.connectCall = [this](int fd, const sockaddr* address,
                                 socklen_t length) {
      return connectPending(fd, address, length);
    };

    {
      health::TcpHealthChecker current({Endpoint{{127, 0, 0, 1}, 1}}, options);
      checker = &current;

      current.setHealthChangeCallback(
          [this](const health::HealthChange& change) {
            onHealthChange(change);
          });

      current.pollHealthProbes();
      requireLifecycle(current.activeProbeCount() == 1,
                       "health pending probe established");
    }

    requireLifecycle(
        notifications == 1 && countOpenFds() == fdBaseline,
        "health destructor cancels pending without fake failure event");

    std::cout << "health threshold/state/fd/cancellation order verified\n";
  }
};

struct NullableSelectionFixture {
  std::string captureSnapshots(bool failSelection) {
    Config config;
    config.protocol = Protocol::kTcp;
    config.metrics = MetricsKind::kStderr;
    config.backends = {Endpoint{{127, 0, 0, 1}, 1}};
    config.healthCheck =
        failSelection ? static_cast<HealthCheck>(77) : HealthCheck::kOff;

    Fd savedStderr(dup(STDERR_FILENO));
    Fd output(open("/tmp", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600));
    requireLifecycle(savedStderr.fd() >= 0 && output.fd() >= 0,
                     "metrics capture descriptors");
    requireLifecycle(dup2(output.fd(), STDERR_FILENO) >= 0,
                     "metrics capture stderr");

    try {
      std::unique_ptr<HealthSelection> selection;
      MetricsService metrics(config, selection);

      if (failSelection) {
        bool initializationFailed = false;
        try {
          selection = std::make_unique<HealthSelection>(config);
        } catch (const std::invalid_argument&) {
          initializationFailed = true;
          metrics.finishMetrics(true);
          metrics.finishMetrics(true);
        }
        requireLifecycle(initializationFailed && !selection,
                         "selection construction leaves nullable holder empty");
      } else {
        metrics.emitReadySnapshot();

        selection = std::make_unique<HealthSelection>(config);

        metrics.finishMetrics(false);
        metrics.finishMetrics(false);
      }
    } catch (...) {
      dup2(savedStderr.fd(), STDERR_FILENO);
      throw;
    }

    requireLifecycle(dup2(savedStderr.fd(), STDERR_FILENO) >= 0,
                     "metrics stderr restored");

    requireLifecycle(lseek(output.fd(), 0, SEEK_SET) == 0,
                     "metrics capture rewind");
    std::string snapshots;
    std::array<char, 4096> buffer{};
    for (;;) {
      auto count = read(output.fd(), buffer.data(), buffer.size());
      requireLifecycle(count >= 0, "metrics capture read");
      if (!count) break;
      snapshots.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return snapshots;
  }

  void exercise() {
    auto baseline = countOpenFds();
    auto normal = captureSnapshots(false);
    requireLifecycle(
        std::count(normal.begin(), normal.end(), '\n') == 2 &&
            normal.find("\"phase\":\"ready\"") != std::string::npos &&
            normal.find("\"phase\":\"final\"") != std::string::npos &&
            normal.find("\"health\":\"disabled\"") != std::string::npos,
        "empty holder ready and assigned holder final exactly once");

    auto failed = captureSnapshots(true);
    requireLifecycle(
        std::count(failed.begin(), failed.end(), '\n') == 1 &&
            failed.find("\"phase\":\"error\"") != std::string::npos &&
            failed.find("\"errors_total\":1") != std::string::npos &&
            failed.find("\"health\":\"Unknown\",\"eligible\":false") !=
                std::string::npos,
        "failed selection constructor uses nullable error fallback once");

    requireLifecycle(countOpenFds() == baseline,
                     "metrics/selection borrow scope leaves no descriptors");

    std::cout << "nullable selection ready/final/error lifetime verified\n";
  }
};

}  // namespace

}  // namespace l4lb::net

int main() {
  try {
    for (bool detachFailure : {false, true}) {
      l4lb::net::ReactorTestAccess tcp;
      tcp.exercise(detachFailure);

      l4lb::net::UdpTestAccess udp;
      udp.exercise(detachFailure);
    }

    l4lb::net::HealthReceiverFixture health;
    health.exercise();

    l4lb::net::NullableSelectionFixture nullableSelection;
    nullableSelection.exercise();

    std::cout << "callback lifecycle mechanism verified\n";
    return 0;
  } catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
  }
}
