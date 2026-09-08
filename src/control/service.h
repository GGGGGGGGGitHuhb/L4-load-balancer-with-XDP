#pragma once
#include "config/config.h"
namespace l4lb {
/** 按已校验协议分派 TCP/UDP；未知协议在网络资源创建前拒绝。 */
int run_service(const Config& config);
}  // namespace l4lb
