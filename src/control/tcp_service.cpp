#include "control/tcp_service.h"

#include <iostream>
#include <stdexcept>
#include <system_error>

#include "control/metrics_service.h"
#include "net/reactor.h"

namespace l4lb {
namespace {
std::string endpoint_text(const Endpoint& e) {
  return std::to_string(e.address[0]) + "." + std::to_string(e.address[1]) +
         "." + std::to_string(e.address[2]) + "." +
         std::to_string(e.address[3]) + ":" + std::to_string(e.port);
}
}  // namespace

int run_tcp_service(const Config& config) {
  if (config.protocol != Protocol::kTcp)
    throw std::invalid_argument("TCP 入口仅支持 TCP 协议");
  std::unique_ptr<HealthSelection> selection;
  MetricsService metrics(config, selection);
  try {
    selection = std::make_unique<HealthSelection>(config);
    net::Callbacks callbacks;
    callbacks.select_backend = [&] { return selection->next(); };
    if (metrics.enabled())
      callbacks.statistics = [&](net::StatEvent e) noexcept {
        metrics.event(e);
      };
    if (selection->enabled() || metrics.enabled())
      callbacks.maintenance = [&] {
        if (selection->enabled()) selection->tick();
        metrics.maintenance();
      };
    callbacks.ready = [&] {
      std::cout << "TCP 服务已启动：" << endpoint_text(config.listen)
                << std::endl;
      metrics.ready();
    };
    callbacks.session = [](const net::SessionEvent& e) {
      std::cerr << "session=" << e.id << " backend=" << endpoint_text(e.backend)
                << " reason=" << e.reason;
      if (!e.accepted)
        std::cerr << " sent_c2b=" << e.sent[0] << " sent_b2c=" << e.sent[1];
      if (e.error)
        std::cerr << " errno=" << e.error << " "
                  << std::generic_category().message(e.error);
      std::cerr << '\n';
    };
    callbacks.diagnostic = [](const std::string& operation, int error) {
      std::cerr << operation << " errno=" << error << " "
                << std::generic_category().message(error) << '\n';
    };
    auto result = net::run(config.listen, callbacks);
    std::cerr << "TCP 服务已停止：全部会话已关闭（不保证在途数据排空）\n";
    metrics.finish(false);
    return result;
  } catch (...) {
    metrics.finish(true);
    throw;
  }
}
}  // namespace l4lb
