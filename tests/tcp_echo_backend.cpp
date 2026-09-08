#include <arpa/inet.h>

#include <charconv>
#include <iostream>
#include <string_view>

#include "net/fd.h"
/** 前台演示 fixture；Ctrl+C 结束，不创建后台子进程。 */
int main(int argc, char** argv) {
  int port = 0;
  if (argc != 2) return 2;
  std::string_view value(argv[1]);
  auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), port);
  if (error != std::errc{} || end != value.data() + value.size() || port < 1 ||
      port > 65535)
    return 2;
  l4lb::net::Fd listener(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  int yes = 1;
  setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (bind(listener.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
          0 ||
      listen(listener.get(), 128) < 0) {
    std::cerr << "echo bind/listen errno=" << errno << '\n';
    return 1;
  }
  std::cout << "echo ready 127.0.0.1:" << port << std::endl;
  for (;;) {
    l4lb::net::Fd client(
        accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
    if (client.get() < 0) {
      if (errno == EINTR) continue;
      return 1;
    }
    char bytes[65536];
    for (;;) {
      auto n = recv(client.get(), bytes, sizeof(bytes), 0);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;
      ssize_t sent = 0;
      while (sent < n) {
        auto count = send(client.get(), bytes + sent, n - sent, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        sent += count;
      }
      if (sent < n) break;
    }
    shutdown(client.get(), SHUT_WR);
  }
}
