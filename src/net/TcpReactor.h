#pragma once
#include <sys/socket.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "config/Config.h"
#include "net/Statistics.h"
#include "net/TcpState.h"

namespace l4lb::net {
/** 结构化生命周期输出；net 不打印日志或载荷。 */
struct SessionEvent {
  std::uint64_t id;
  Endpoint backend;
  bool accepted;
  std::string reason;
  int error = 0;
  std::uint64_t sent[2]{};
};

/** 内部观测，不是产品指标接口。未设置时没有逐 I/O 输出。 */
struct Observation {
  std::string kind;
  std::uint64_t session = 0;
  int side = -1;
  std::uint64_t value = 0;
  std::size_t sessions = 0, tokens = 0;
};

struct TcpReactorOptions {
  std::size_t maxSessions = 1024;
  std::chrono::milliseconds connectTimeout{5000}, idleTimeout{60000};
  std::chrono::milliseconds drainTimeout{1000};

  using ObservationCallback = std::function<void(const Observation&)>;

  void setObservationCallback(ObservationCallback observationCallback) {
    observationCallback_ = std::move(observationCallback);
  }

  const ObservationCallback& observationCallback() const {
    return observationCallback_;
  }

  // 故障注入仅用于内部测试；缺省始终调用真实系统接口。
  std::function<int(int, const sockaddr*, socklen_t)> connectCall;
  std::function<int(int, int*)> socketErrorCall;
  std::function<ssize_t(int, const void*, std::size_t, int)> sendCall;
  std::function<int(int, sockaddr*, socklen_t*, int)> acceptCall;

 private:
  ObservationCallback observationCallback_;
};

/** 停止屏障只报告已在用户态的队列，不代表内核中在途数据。 */
struct StopEvent {
  std::uint64_t pending[2]{};
  std::int64_t deadlineNs = 0;
};

class TcpReactorCallbacks {
 public:
  using BackendSelector = std::function<std::optional<Endpoint>()>;

  using MaintenanceCallback = std::function<void()>;
  using StatisticsCallback = std::function<void(StatEvent)>;
  using ReadyCallback = std::function<void()>;
  using StoppingCallback = std::function<void(const StopEvent&)>;
  using SessionCallback = std::function<void(const SessionEvent&)>;
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

  void setStoppingCallback(StoppingCallback stoppingCallback) {
    stoppingCallback_ = std::move(stoppingCallback);
  }

  const StoppingCallback& stoppingCallback() const { return stoppingCallback_; }

  void setSessionCallback(SessionCallback sessionCallback) {
    sessionCallback_ = std::move(sessionCallback);
  }

  const SessionCallback& sessionCallback() const { return sessionCallback_; }

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
  StoppingCallback stoppingCallback_;
  SessionCallback sessionCallback_;
  DiagnosticCallback diagnosticCallback_;
};

/** 单线程 LT reactor，信号退出返回 0；不可恢复错误抛 system_error。 */
int runTcpReactor(const Endpoint& listen, const TcpReactorCallbacks& callbacks,
                  const TcpReactorOptions& options = {});
}  // namespace l4lb::net
