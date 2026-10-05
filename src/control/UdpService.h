#pragma once
#include "config/Config.h"

namespace l4lb {
/** 防御非 UDP 配置，持有调度器并同步运行 UDP reactor。 */
int runUdpService(const Config& config);
}  // namespace l4lb
