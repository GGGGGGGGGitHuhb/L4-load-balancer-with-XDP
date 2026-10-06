#pragma once
#include <sys/socket.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "config/Config.h"
#include "core/UdpFlow.h"
#include "net/Statistics.h"

namespace l4lb::net {
inline constexpr std::size_t kUdpPayloadLimit = 65507;

struct UdpFlowEvent {
  std::uint64_t id;
  Endpoint backend;
  std::string reason;
  int error = 0;
};

struct UdpObservation {
  std::string kind;
  std::uint64_t token;
  int fd;
  std::size_t flows, tokens;
};

class UdpReactorCallbacks {
 public:
  using BackendSelector = std::function<std::optional<Endpoint>()>;

  using MaintenanceCallback = std::function<void()>;
  using StatisticsCallback = std::function<void(StatEvent)>;
  using ReadyCallback = std::function<void()>;
  using FlowCallback = std::function<void(const UdpFlowEvent&)>;
  using DiagnosticCallback = std::function<void(const std::string&, int)>;

  void setBackendSelector(BackendSelector backendSelector) {
    backendSelector_ = std::move(backendSelector);
  }

  const BackendSelector& backendSelector() const { return backendSelector_; }

  void setMaintenanceCallback(MaintenanceCallback maintenanceCallback) {
    maintenanceCallback_ = std::move(maintenanceCallback);
  }

  const MaintenanceCallback& maintenanceCallback() const {
    return maintenanceCallback_;
  }

  void setStatisticsCallback(StatisticsCallback statisticsCallback) {
    statisticsCallback_ = std::move(statisticsCallback);
  }

  const StatisticsCallback& statisticsCallback() const {
    return statisticsCallback_;
  }

  void setReadyCallback(ReadyCallback readyCallback) {
    readyCallback_ = std::move(readyCallback);
  }

  const ReadyCallback& readyCallback() const { return readyCallback_; }

  void setFlowCallback(FlowCallback flowCallback) {
    flowCallback_ = std::move(flowCallback);
  }

  const FlowCallback& flowCallback() const { return flowCallback_; }

  void setDiagnosticCallback(DiagnosticCallback diagnosticCallback) {
    diagnosticCallback_ = std::move(diagnosticCallback);
  }

  const DiagnosticCallback& diagnosticCallback() const {
    return diagnosticCallback_;
  }

 private:
  BackendSelector backendSelector_;

  MaintenanceCallback maintenanceCallback_;
  StatisticsCallback statisticsCallback_;
  ReadyCallback readyCallback_;
  FlowCallback flowCallback_;
  DiagnosticCallback diagnosticCallback_;
};

/** 内部测试选项；生产不暴露配置项。空注入函数均使用真实 syscall。 */
struct UdpReactorOptions {
  std::size_t maxFlows = 1024;
  std::chrono::milliseconds idleTimeout{60000}, pollInterval{100};
  std::size_t receiveSize = kUdpPayloadLimit;

  using ObservationCallback = std::function<void(const UdpObservation&)>;

  void setObservationCallback(ObservationCallback observationCallback) {
    observationCallback_ = std::move(observationCallback);
  }

  const ObservationCallback& observationCallback() const {
    return observationCallback_;
  }

  // 返回 0 继续真实调用，非零值模拟该操作 errno。仅 flow 建立事务使用。
  std::function<int(const char*, int)> setupError;
  std::function<ssize_t(int, const msghdr*, int)> sendmsgCall;
  std::function<ssize_t(int, msghdr*, int)> recvmsgCall;
  std::function<int(int, int*)> socketErrorCall;

  std::function<UdpIdleDeadline::Clock::time_point()> now;

 private:
  ObservationCallback observationCallback_;
};

/** 同步单线程 LT UDP reactor；信号退出 0，不可恢复错误抛异常。 */
int runUdpReactor(const Endpoint& listen, const UdpReactorCallbacks& callbacks,
                  const UdpReactorOptions& options = {});
}  // namespace l4lb::net
