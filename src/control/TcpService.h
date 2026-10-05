#pragma once
#include "config/Config.h"

namespace l4lb {
/** 防御非 TCP 协议，组装配置策略和服务日志；异常诊断由 CLI 展示。 */
int runTcpService(const Config& config);
}  // namespace l4lb
