#pragma once

#include <signal.h>

#include <string>

#include "xdp/XdpAttachment.h"

namespace l4lb::control {
/** 运行可选的动态模式，直到收到停止信号或发生致命错误。 */
int runRuntimeDsrControlLoop(xdp::XdpAttachment& attachment,
                             const std::string& object,
                             const std::string& ingress, const std::string& vip,
                             const std::string& configurationPath,
                             xdp::XdpAttachMode mode, const sigset_t& signals);
}  // namespace l4lb::control
