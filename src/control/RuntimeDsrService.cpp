#include "RuntimeDsrService.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>

#include "RuntimeDsrConfig.h"
#include "health/UdpProbeChecker.h"
#include "net/Fd.h"

namespace l4lb::control {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

class RuntimeControlError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/** 仅 runtime 模式修改 stdout 标志，并恢复继承的状态。 */
class RuntimeOutput {
 public:
  RuntimeOutput() : originalFlags_(fcntl(STDOUT_FILENO, F_GETFL)) {
    if (originalFlags_ < 0 ||
        fcntl(STDOUT_FILENO, F_SETFL, originalFlags_ | O_NONBLOCK))
      throw std::runtime_error("无法设置 runtime 非阻塞输出");
  }

  ~RuntimeOutput() { fcntl(STDOUT_FILENO, F_SETFL, originalFlags_); }

  void appendOutputLine(const std::string& line) {
    if (pending_.size() + line.size() + 1 > 64 * 1024)
      throw RuntimeControlError("runtime 输出队列超过64KiB");

    pending_ += line;
    pending_ += '\n';

    flushPendingOutput();
  }

  void flushPendingOutput() {
    for (unsigned attempt = 0; attempt < 64 && !pending_.empty(); ++attempt) {
      const auto count = write(STDOUT_FILENO, pending_.data(), pending_.size());
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
      if (count <= 0) throw RuntimeControlError("runtime 输出失败");
      pending_.erase(0, static_cast<size_t>(count));
    }
  }

  bool hasPendingOutput() const { return !pending_.empty(); }

  void drainOutputOnStop() {
    const auto deadline = Clock::now() + 1s;
    while (hasPendingOutput()) {
      flushPendingOutput();
      if (!hasPendingOutput()) break;

      if (Clock::now() >= deadline)
        throw RuntimeControlError("停止后 runtime 输出背压超时");

      pollfd output{STDOUT_FILENO, POLLOUT, 0};
      if (poll(&output, 1, 20) < 0 && errno != EINTR)
        throw RuntimeControlError("runtime 输出等待失败");
    }
  }

 private:
  int originalFlags_;
  std::string pending_;
};

struct SignalState {
  bool reload = false;
  bool stop = false;
  bool brokenPipe = false;
};

void consumeRuntimeSignals(int fd, SignalState& state) {
  sigset_t stopSignals;
  sigemptyset(&stopSignals);
  sigaddset(&stopSignals, SIGINT);
  sigaddset(&stopSignals, SIGTERM);
  sigaddset(&stopSignals, SIGPIPE);

  const timespec noWait{};
  const int stopped = sigtimedwait(&stopSignals, nullptr, &noWait);
  if (stopped > 0) {
    state.stop = true;
    state.brokenPipe |= stopped == SIGPIPE;
    return;
  }

  if (errno != EAGAIN && errno != EINTR)
    throw RuntimeControlError("runtime 停止信号检查失败");

  for (unsigned count = 0; count < 64; ++count) {
    signalfd_siginfo information{};
    const auto bytes = read(fd, &information, sizeof(information));
    if (bytes < 0 && errno == EINTR) continue;
    if (bytes < 0 && errno == EAGAIN) return;
    if (bytes != sizeof(information))
      throw RuntimeControlError("runtime 信号读取失败");
    if (information.ssi_signo == SIGHUP)
      state.reload = true;
    else {
      state.stop = true;
      state.brokenPipe |= information.ssi_signo == SIGPIPE;
    }
  }
}

std::vector<health::ProbeTarget> buildProbeTargets(
    const RuntimeDsrConfiguration& configuration) {
  std::vector<health::ProbeTarget> result;
  for (const auto& target : configuration.targets) {
    health::ProbeTarget probe;
    probe.id = target.id;
    probe.interfaceName = target.egress;
    probe.ifindex = target.backend.ifindex;
    std::copy_n(target.backend.sourceMac, 6, probe.sourceMac.begin());
    std::copy_n(target.backend.destinationMac, 6, probe.destinationMac.begin());
    probe.probeAddress = target.probeAddress;
    probe.probePort = target.probePort;
    result.push_back(std::move(probe));
  }

  return result;
}

UdpRuntimeSnapshot buildRuntimeSnapshot(
    const RuntimeDsrConfiguration& configuration,
    const health::UdpProbeChecker& checker, uint64_t generation) {
  UdpRuntimeSnapshot snapshot{};
  snapshot.schemaVersion = L4LB_RUNTIME_SCHEMA_VERSION;
  snapshot.generation = generation;
  snapshot.vipAddress = configuration.vipAddress;
  snapshot.vipPort = configuration.vipPort;
  for (const auto& target : checker.copyHealthyProbeTargets()) {
    if (snapshot.backendCount >= L4LB_DSR_MAX_BACKENDS)
      throw std::logic_error("runtime 健康目标超界");
    auto& backend = snapshot.backends[snapshot.backendCount++];
    backend.ifindex = target.ifindex;
    std::copy(target.sourceMac.begin(), target.sourceMac.end(),
              backend.sourceMac);
    std::copy(target.destinationMac.begin(), target.destinationMac.end(),
              backend.destinationMac);
  }

  return snapshot;
}

uint64_t nextRuntimeGeneration(const UdpRuntimeSnapshot& applied) {
  if (applied.generation == std::numeric_limits<uint64_t>::max())
    throw RuntimeControlError("runtime generation 已耗尽");
  return applied.generation + 1;
}

bool haveSameActiveTargets(const UdpRuntimeSnapshot& first,
                           const UdpRuntimeSnapshot& second) {
  return first.backendCount == second.backendCount &&
         std::memcmp(first.backends, second.backends, sizeof(first.backends)) ==
             0;
}

std::string formatAppliedSnapshotFields(const UdpRuntimeSnapshot& applied) {
  return " generation=" + std::to_string(applied.generation) +
         " active_backends=" + std::to_string(applied.backendCount);
}

std::string sanitizeErrorReason(const std::exception& error) {
  std::string reason = error.what();
  for (char& character : reason)
    if (static_cast<unsigned char>(character) <= 32) character = '_';
  if (reason.size() > 512) reason.resize(512);
  return reason;
}

std::string formatRuntimeStatisticsLine(const UdpDsrStatsValue& stats) {
  std::ostringstream line;
  line << "XDP_STATS schema=3 total_packets=" << stats.totalPackets
       << " pass_packets=" << stats.passPackets
       << " redirect_requests=" << stats.redirectRequests
       << " drop_packets=" << stats.dropPackets
       << " unsupported_packets=" << stats.unsupportedPackets
       << " no_backend_packets=" << stats.noBackendPackets
       << " invalid_config_packets=" << stats.invalidConfigPackets
       << " helper_error_packets=" << stats.helperErrorPackets;

  return line.str();
}
}  // namespace

int runRuntimeDsrControlLoop(xdp::XdpAttachment& attachment,
                             const std::string& object,
                             const std::string& ingress, const std::string& vip,
                             const std::string& configurationPath,
                             xdp::XdpAttachMode mode, const sigset_t& signals) {
  const auto path = std::filesystem::absolute(configurationPath).string();

  auto configuration = loadRuntimeConfig(path, ingress, vip);

  auto checker = std::make_unique<health::UdpProbeChecker>(
      buildProbeTargets(configuration));

  auto applied = buildRuntimeSnapshot(configuration, *checker, 1);

  net::Fd signalFd(signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC));
  if (signalFd.fd() < 0) throw std::runtime_error("无法创建 runtime signalfd");

  RuntimeOutput output;

  attachment.loadObject(object, xdp::XdpObjectProfile::kUdpRuntimeV3, {}, {},
                        &applied);
  attachment.attachProgram(xdp::resolveInterfaceIndex(ingress), mode);

  try {
    output.appendOutputLine(
        "READY dev=" + ingress + " mode=" + xdp::xdpAttachModeName(mode) +
        " prog_id=" + std::to_string(attachment.programId()) +
        " schema=3 profile=udp-dsr-runtime configured_backends=" +
        std::to_string(configuration.targets.size()) +
        formatAppliedSnapshotFields(applied));

    SignalState signalState;
    auto nextPublish = Clock::now() + 1s;

    while (!signalState.stop) {
      consumeRuntimeSignals(signalFd.fd(), signalState);
      if (signalState.stop) break;

      const auto now = Clock::now();

      for (const auto& transition : checker->pollProbeTransitions(now)) {
        output.appendOutputLine(
            "XDP_HEALTH target=" + transition.id +
            " from=" + health::probeStateName(transition.from) +
            " to=" + health::probeStateName(transition.to) +
            " reason=" + transition.reason);
      }
      consumeRuntimeSignals(signalFd.fd(), signalState);
      if (signalState.stop) break;

      if (signalState.reload && now >= nextPublish) {
        signalState.reload = false;

        // 所有可能抛异常的准备步骤仍在唯一的 map 提交之前。
        bool committed = false;
        bool publishAttempted = false;

        try {
          auto candidate = loadRuntimeConfig(path, ingress, vip);
          if (sameRuntimeConfig(candidate, configuration)) {
            output.appendOutputLine("XDP_RELOAD status=unchanged" +
                                    formatAppliedSnapshotFields(applied));
          } else {
            auto candidateChecker = std::make_unique<health::UdpProbeChecker>(
                buildProbeTargets(candidate), checker->copyProbeSnapshots());
            auto snapshot = buildRuntimeSnapshot(
                candidate, *candidateChecker, nextRuntimeGeneration(applied));
            consumeRuntimeSignals(signalFd.fd(), signalState);
            if (signalState.stop) break;

            nextPublish = Clock::now() + 1s;
            publishAttempted = true;

            attachment.publishRuntime(snapshot);

            committed = true;
            applied = snapshot;
            configuration = std::move(candidate);
            checker = std::move(candidateChecker);

            output.appendOutputLine("XDP_PUBLISH status=applied reason=reload" +
                                    formatAppliedSnapshotFields(applied));

            output.appendOutputLine(
                "XDP_RELOAD status=applied configured_backends=" +
                std::to_string(configuration.targets.size()) +
                formatAppliedSnapshotFields(applied));
          }
        } catch (const xdp::RuntimePublishError& error) {
          if (error.committed()) throw;

          output.appendOutputLine("XDP_PUBLISH status=failed reason=reload" +
                                  formatAppliedSnapshotFields(applied));

          output.appendOutputLine("XDP_RELOAD status=rejected reason=" +
                                  sanitizeErrorReason(error) +
                                  formatAppliedSnapshotFields(applied));
        } catch (const RuntimeControlError&) {
          throw;
        } catch (const std::exception& error) {
          if (committed) throw;
          if (publishAttempted)
            output.appendOutputLine("XDP_PUBLISH status=failed reason=reload" +
                                    formatAppliedSnapshotFields(applied));

          output.appendOutputLine("XDP_RELOAD status=rejected reason=" +
                                  sanitizeErrorReason(error) +
                                  formatAppliedSnapshotFields(applied));
        }
      }

      if (Clock::now() >= nextPublish) {
        auto snapshot = buildRuntimeSnapshot(configuration, *checker,
                                             nextRuntimeGeneration(applied));
        if (!haveSameActiveTargets(snapshot, applied)) {
          consumeRuntimeSignals(signalFd.fd(), signalState);
          if (signalState.stop) break;

          nextPublish = Clock::now() + 1s;
          bool committed = false;

          try {
            attachment.publishRuntime(snapshot);

            committed = true;
            applied = snapshot;

            output.appendOutputLine("XDP_PUBLISH status=applied reason=health" +
                                    formatAppliedSnapshotFields(applied));
          } catch (const xdp::RuntimePublishError& error) {
            if (error.committed()) throw;

            output.appendOutputLine("XDP_PUBLISH status=failed reason=health" +
                                    formatAppliedSnapshotFields(applied));
          } catch (const RuntimeControlError&) {
            throw;
          } catch (const std::exception&) {
            if (committed) throw;

            output.appendOutputLine("XDP_PUBLISH status=failed reason=health" +
                                    formatAppliedSnapshotFields(applied));
          }
        }
      }

      output.flushPendingOutput();

      pollfd descriptors[2]{
          {signalFd.fd(), POLLIN, 0},
          {STDOUT_FILENO,
           static_cast<short>(output.hasPendingOutput() ? POLLOUT : 0), 0}};
      if (poll(descriptors, 2, 20) < 0 && errno != EINTR)
        throw RuntimeControlError("runtime poll 失败");
      if ((descriptors[0].revents | descriptors[1].revents) &
          (POLLERR | POLLHUP | POLLNVAL))
        throw RuntimeControlError("runtime 输出或信号描述符失效");
    }

    attachment.detachProgram();

    checker.reset();

    output.appendOutputLine(
        formatRuntimeStatisticsLine(attachment.readRuntimeStats()));

    output.appendOutputLine(
        "DETACHED dev=" + ingress + " mode=" + xdp::xdpAttachModeName(mode) +
        " prog_id=" + std::to_string(attachment.programId()));

    output.drainOutputOnStop();

    return signalState.brokenPipe ? 1 : 0;
  } catch (...) {
    // 在 main 中可能阻塞的诊断输出之前完成清理。
    try {
      attachment.detachProgram();
    } catch (...) {
    }

    throw;
  }
}
}  // namespace l4lb::control
