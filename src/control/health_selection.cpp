#include "control/health_selection.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace l4lb {
std::optional<std::size_t> eligible_next(Scheduler& scheduler,
                                         const std::vector<bool>& eligible) {
  if (std::none_of(eligible.begin(), eligible.end(),
                   [](bool value) { return value; }))
    return std::nullopt;
  for (std::size_t n = 0; n < eligible.size(); ++n) {
    auto i = scheduler.next();
    if (eligible.at(i)) return i;
  }
  throw std::logic_error("scheduler did not visit eligible backend");
}

HealthSelection::HealthSelection(const Config& config)
    : config_(config),
      scheduler_(make_scheduler(config.scheduler, config.backends.size())) {
  if (config.health_check == HealthCheck::kOff) return;
  if (config.health_check != HealthCheck::kTcpConnect)
    throw std::invalid_argument("invalid health_check");
  checker_ = std::make_unique<health::Checker>(
      config.backends, [&](const health::Change& c) {
        const auto& e = config_.backends[c.backend];
        std::cerr << "health backend=" << unsigned(e.address[0]) << '.'
                  << unsigned(e.address[1]) << '.' << unsigned(e.address[2])
                  << '.' << unsigned(e.address[3]) << ':' << e.port
                  << " from=" << health::name(c.from)
                  << " to=" << health::name(c.to) << " reason=" << c.reason;
        if (c.error) std::cerr << " errno=" << c.error;
        std::cerr << '\n';
      });
  if (config.protocol == Protocol::kUdp)
    std::cerr << "UDP health_check=tcp_connect：相同 IP/端口的 TCP "
                 "健康端点必须代表 UDP 服务；TCP 握手不是 UDP 协议健康证明。\n";
}

std::optional<Endpoint> HealthSelection::next() {
  if (!checker_) return config_.backends[scheduler_->next()];
  checker_->tick();  // 长批次内每次新选择仍先处理当前截止和事件。
  std::vector<bool> eligible(checker_->size());
  for (std::size_t i = 0; i < eligible.size(); ++i)
    eligible[i] = checker_->state(i).status == health::Status::Healthy;
  auto index = eligible_next(*scheduler_, eligible);
  if (index) return config_.backends[*index];
  auto time = health::Clock::now();
  if (!unavailable_log_ ||
      time - *unavailable_log_ >= std::chrono::seconds(1)) {
    std::cerr << "no_healthy_backend\n";
    unavailable_log_ = time;
  }
  return std::nullopt;
}
}  // namespace l4lb
