#include <arpa/inet.h>

#include <charconv>
#include <iostream>
#include <string_view>

#include "net/fd.h"
/** 前台演示 fixture；Ctrl+C 结束，不创建后台子进程。 */
int main(int argc, char** argv) {
  int port = 0;
  if (argc != 2 && argc != 3) return 2;
  // 可选测试标记：对回包逐字节 XOR，以数据证明选中了哪个后端。
  unsigned mask = 0;
  if (argc == 3) {
    std::string_view marker(argv[2]);
    auto [last, code] =
        std::from_chars(marker.data(), marker.data() + marker.size(), mask);
    if (code != std::errc{} || last != marker.data() + marker.size() ||
        mask > 255)
      return 2;
  }
  std::string_view value(argv[1]);
  auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), port);
  if (error != std::errc{} || end != value.data() + value.size() || port < 0 ||
      port > 65535)
    return 2;
  l4lb::net::Fd listener(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  int yes = 1;
  if (listener.get() < 0 || setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
                                       &yes, sizeof(yes)) < 0) {
    std::cerr << "echo socket/setsockopt errno=" << errno << '\n';
    return 1;
  }
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
  socklen_t length = sizeof(addr);
  if (getsockname(listener.get(), reinterpret_cast<sockaddr*>(&addr), &length) <
      0) {
    std::cerr << "echo getsockname errno=" << errno << '\n';
    return 1;
  }
  port = ntohs(addr.sin_port);
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
      for (ssize_t i = 0; i < n; ++i) bytes[i] ^= mask;
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
