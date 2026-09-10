#pragma once
#include "config/config.h"

namespace l4lb {
/** 防御非 UDP 配置，持有调度器并同步运行 UDP reactor。 */
int run_udp_service(const Config& config);
}  // namespace l4lb
