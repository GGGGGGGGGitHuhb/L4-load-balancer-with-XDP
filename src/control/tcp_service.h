#pragma once
#include "config/config.h"
namespace l4lb {
/** 组装固定轮询和服务日志；异常诊断由 CLI 展示。 */
int run_tcp_service(const Config& config);
}  // namespace l4lb
