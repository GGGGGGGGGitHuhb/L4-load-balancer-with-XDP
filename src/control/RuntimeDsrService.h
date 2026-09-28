#pragma once

#include <signal.h>

#include <string>

#include "xdp/loader.h"

namespace l4lb::control {
/** Runs the optional dynamic profile until a stop signal or fatal error. */
int runRuntimeDsr(xdp::Attachment& attachment, const std::string& object,
                  const std::string& ingress, const std::string& vip,
                  const std::string& configurationPath, xdp::Mode mode,
                  const sigset_t& signals);
}  // namespace l4lb::control
