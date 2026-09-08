#include "core/scheduler.h"

#include <iostream>
#include <stdexcept>
#include <type_traits>

#include "control/service.h"
#include "control/tcp_service.h"
#include "control/udp_service.h"
namespace {
int checks = 0;
void check(bool ok) {
  ++checks;
  if (!ok) throw std::runtime_error("scheduler check failed");
}
template <class F>
void rejected(F f) {
  bool threw = false;
  try {
    f();
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  check(threw);
}
}  // namespace
int main(int argc, char**) {
  try {
    if (argc > 1) check(false);
    check(std::has_virtual_destructor_v<l4lb::Scheduler>);
    auto kind = l4lb::SchedulerKind::kRoundRobin;
    for (std::size_t size : {1, 3, 256}) {
      auto first = l4lb::make_scheduler(kind, size);
      auto other = l4lb::make_scheduler(kind, 3);
      for (std::size_t i = 0; i < size * 12; ++i) {
        check(first->next() == i % size);
        if (i % 2 == 0) check(other->next() == (i / 2) % 3);
      }
      auto restarted = l4lb::make_scheduler(kind, size);
      check(restarted->next() == 0);
    }
    rejected([&] { l4lb::make_scheduler(kind, 0); });
    rejected(
        [&] { l4lb::make_scheduler(static_cast<l4lb::SchedulerKind>(99), 3); });
    // 空池证明协议拒绝早于调度创建，更早于 reactor 资源获取。
    l4lb::Config config;
    config.protocol = l4lb::Protocol::kUdp;
    for (auto entry : {l4lb::run_tcp_service}) {
      try {
        entry(config);
        check(false);
      } catch (const std::invalid_argument& e) {
        check(std::string(e.what()).find("协议") != std::string::npos ||
              std::string(e.what()).find("UDP 转发") != std::string::npos);
      }
    }
    config.protocol = l4lb::Protocol::kTcp;
    rejected([&] { l4lb::run_udp_service(config); });
    config.protocol = static_cast<l4lb::Protocol>(99);
    rejected([&] { l4lb::run_service(config); });
    rejected([&] { l4lb::run_tcp_service(config); });
    rejected([&] { l4lb::run_udp_service(config); });
    std::cout << "PASS scheduler/control checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
