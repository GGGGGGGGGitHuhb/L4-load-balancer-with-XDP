#pragma once
#include "config/config.h"
namespace l4lb {
/** 在任何网络资源创建前分派协议；当前 UDP 抛 invalid_argument。 */
int run_service(const Config& config);
}  // namespace l4lb
