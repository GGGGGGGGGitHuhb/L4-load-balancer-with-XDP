#include <sys/socket.h>

#include <cerrno>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

#include "config/config.h"
#include "core/round_robin.h"
#include "net/reactor.h"

/** 内部集成驱动：和产品链接同一 reactor，不增加产品 CLI 参数。 */
int main(int argc, char** argv) {
  if (argc != 4) return 2;
  auto result = l4lb::load_config(argv[1]);
  auto* config = std::get_if<l4lb::Config>(&result);
  if (!config) return 1;
  std::ofstream trace(argv[2]);
  std::string mode(argv[3]);
  l4lb::RoundRobin rr(config->backends.size());
  l4lb::net::Callbacks cb;
  cb.select_backend = [&] { return config->backends[rr.next()]; };
  cb.ready = [] { std::cout << "READY" << std::endl; };
  cb.session = [&](const l4lb::net::SessionEvent& e) {
    trace << (e.accepted ? "accepted" : "ended") << ' ' << e.id << ' '
          << e.backend.port << ' ' << e.reason << ' ' << e.error << ' '
          << e.sent[0] << ' ' << e.sent[1] << std::endl;
  };
  cb.diagnostic = [&](const std::string& name, int error) {
    trace << "diagnostic " << name << ' ' << error << std::endl;
  };
  l4lb::net::Options options;
  options.observe = [&](const l4lb::net::Observation& o) {
    trace << o.kind << ' ' << o.session << ' ' << o.side << ' ' << o.value
          << ' ' << o.sessions << ' ' << o.tokens << std::endl;
  };
  if (mode == "capacity") options.max_sessions = 1;
  if (mode == "idle") options.idle_timeout = std::chrono::milliseconds(250);
  if (mode == "connect-timeout")
    options.connect_timeout = std::chrono::milliseconds(0);
  if (mode == "small-send" || mode == "short-send") {
    options.send_call = [&, initialized = std::set<int>{}](
                            int fd, const void* data, std::size_t size,
                            int flags) mutable {
      if (initialized.insert(fd).second) {
        int value = 4096;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value)) < 0)
          return ssize_t(-1);
      }
      return send(fd, data,
                  mode == "short-send" ? std::min(size, std::size_t(7)) : size,
                  flags);
    };
  }
  if (mode == "accept-backoff") {
    options.accept_call = [count = 0](int fd, sockaddr* addr, socklen_t* size,
                                      int flags) mutable {
      if (count++ < 3) {
        errno = EMFILE;
        return -1;
      }
      return accept4(fd, addr, size, flags);
    };
  }
  try {
    return l4lb::net::run(config->listen, cb, options);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}
