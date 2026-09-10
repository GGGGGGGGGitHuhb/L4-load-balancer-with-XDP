#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "net/fd.h"

using l4lb::net::Fd;
using namespace std::chrono_literals;

namespace {
std::string product, driver, root;
int sequence = 0;

void check(bool ok, const std::string& why) {
  if (!ok) throw std::runtime_error(why);
}

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), {}};
}

void until(const std::function<bool()>& condition, const std::string& why,
           std::chrono::milliseconds limit = 4000ms) {
  auto end = std::chrono::steady_clock::now() + limit;
  while (!condition()) {
    check(std::chrono::steady_clock::now() < end, "timeout: " + why);
    std::this_thread::sleep_for(5ms);
  }
}

sockaddr_in addr(int port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return a;
}

int port_of(int fd) {
  sockaddr_in a{};
  socklen_t n = sizeof(a);
  check(getsockname(fd, reinterpret_cast<sockaddr*>(&a), &n) == 0,
        "getsockname");
  return ntohs(a.sin_port);
}

Fd listener(int port = 0) {
  Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  check(fd.get() >= 0, "socket");
  int yes = 1;
  setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  auto a = addr(port);
  check(bind(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0,
        "bind fixture");
  check(listen(fd.get(), 128) == 0, "listen fixture");
  return fd;
}

void timeout(int fd) {
  timeval tv{12, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

Fd client(int port) {
  Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  check(fd.get() >= 0, "client socket");
  timeout(fd.get());
  auto a = addr(port);
  check(connect(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0,
        "client connect");
  return fd;
}

void send_all(int fd, const std::string& data) {
  std::size_t done = 0;
  while (done < data.size()) {
    auto n = send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) continue;
    check(n > 0, "send fixture errno=" + std::to_string(errno));
    done += n;
  }
}

std::string receive(int fd) {
  std::string out;
  char bytes[65536];
  for (;;) {
    auto n = recv(fd, bytes, sizeof(bytes), 0);
    if (n < 0 && errno == EINTR) continue;
    check(n >= 0, "recv fixture errno=" + std::to_string(errno));
    if (!n) return out;
    out.append(bytes, n);
  }
}

std::string exact(int fd, std::size_t size) {
  std::string out(size, '\0');
  std::size_t n = 0;
  while (n < size) {
    auto got = recv(fd, out.data() + n, size - n, 0);
    check(got > 0, "recv exact");
    n += got;
  }
  return out;
}

std::string binary(std::size_t size) {
  std::string data(size, '\0');
  for (std::size_t i = 0; i < size; ++i)
    data[i] = char((i * 19 + i / 251) % 256);
  return data;
}

struct Child {
  pid_t pid = -1;
  Child() = default;

  explicit Child(pid_t p) : pid(p) {}

  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;

  Child(Child&& other) noexcept : pid(std::exchange(other.pid, -1)) {}

  Child& operator=(Child&& other) noexcept {
    if (this != &other) {
      stop();
      pid = std::exchange(other.pid, -1);
    }
    return *this;
  }

  ~Child() { stop(); }

  void stop() {
    if (pid > 0) {
      kill(pid, SIGKILL);
      int status;
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      pid = -1;
    }
  }

  int finish(int signal = 0) {
    if (signal) check(kill(pid, signal) == 0, "signal child");
    int status = 0;
    until(
        [&] {
          auto rc = waitpid(pid, &status, WNOHANG);
          check(rc >= 0, "waitpid");
          return rc == pid;
        },
        "child exit", 1800ms);
    pid = -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  }
};

Child exec_process(const std::vector<std::string>& args, const std::string& out,
                   const std::string& err) {
  auto pid = fork();
  check(pid >= 0, "fork");
  if (!pid) {
    int o = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600),
        e = open(err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (o < 0 || e < 0) _exit(125);
    dup2(o, 1);
    dup2(e, 2);
    close(o);
    close(e);
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(126);
  }
  return Child(pid);
}

struct Backend {
  int port;
  Child child;
  Fd completion;

  explicit Backend(std::string mode = "echo", char label = 'A') {
    auto server = listener();
    port = port_of(server.get());
    int channel[2];
    check(pipe2(channel, O_CLOEXEC | O_NONBLOCK) == 0,
          "backend completion pipe");
    completion = Fd(channel[0]);
    Fd completion_writer(channel[1]);
    auto pid = fork();
    check(pid >= 0, "backend fork");
    if (!pid) {
      completion.reset();
      for (;;) {
        int fd = accept4(server.get(), nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) {
          if (errno == EINTR) continue;
          _exit(3);
        }
        std::thread([fd, mode, label, status_fd = completion_writer.get()] {
          // 反向 FIN 用例的最后数据只被后端消费，没有响应可以代替该断言。
          // 完成消息必须在检查全部字节成功后发送，父级显式等待。
          auto publish = [status_fd](const std::string& status) {
            const auto message = status + "\n";
            ssize_t n;
            do {
              n = write(status_fd, message.data(), message.size());
            } while (n < 0 && errno == EINTR);
            if (n != static_cast<ssize_t>(message.size())) _exit(5);
          };
          Fd owner(fd);
          timeout(fd);
          try {
            if (mode == "rst") {
              linger value{1, 0};
              setsockopt(fd, SOL_SOCKET, SO_LINGER, &value, sizeof(value));
              return;
            }
            if (mode == "reverse-fin") {
              send_all(fd, std::string("reply\0tail", 10));
              shutdown(fd, SHUT_WR);
              auto request = receive(fd);
              if (request != std::string("after-eof\0data", 14)) {
                publish("FAIL reverse-fin payload mismatch bytes=" +
                        std::to_string(request.size()));
                return;
              }
              publish("OK reverse-fin bytes=14");
              return;
            }
            if (mode == "label") send_all(fd, std::string(1, label));
            if (mode == "slow") std::this_thread::sleep_for(450ms);
            if (mode == "after-eof") {
              auto data = receive(fd);
              send_all(fd, std::string("response\0", 9) + data);
              shutdown(fd, SHUT_WR);
              return;
            }
            char bytes[32768];
            for (;;) {
              auto n = recv(fd, bytes, sizeof(bytes), 0);
              if (n < 0 && errno == EINTR) continue;
              if (n <= 0) break;
              send_all(fd, std::string(bytes, n));
            }
            shutdown(fd, SHUT_WR);
          } catch (const std::exception& error) {
            if (mode == "reverse-fin")
              publish(std::string("FAIL reverse-fin ") + error.what());
            // echo/slow/label/after-eof 的完整响应由父级逐字节验证；
            // 主动关闭/RST/服务停止时允许这些 fixture 的 I/O 失败。
          } catch (...) {
            if (mode == "reverse-fin")
              publish("FAIL reverse-fin unknown exception");
          }
        }).detach();
      }
    }
    child = Child(pid);
  }

  void verify_reverse_fin() {
    std::string status;
    until(
        [&] {
          char bytes[256];
          auto n = read(completion.get(), bytes, sizeof(bytes));
          if (n > 0)
            status.append(bytes, n);
          else if (n == 0) {
            const int code = child.finish();
            throw std::runtime_error(
                "reverse-fin backend exited before completion: code=" +
                std::to_string(code));
          } else {
            check(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR,
                  "reverse-fin completion read");
          }
          return status.find('\n') != std::string::npos;
        },
        "reverse-fin backend completion", 2000ms);
    check(status == "OK reverse-fin bytes=14\n",
          "backend assertion: " + status);
  }
};

struct Service {
  int port = 0;
  std::string base, config, out, err, trace, original;
  Child child;

  Service(const std::vector<int>& backends, std::string mode = "product",
          int fixed_port = 0, bool expect_ready = true) {
    for (int attempt = 0; attempt < 5; ++attempt) {
      base = root + "/case-" + std::to_string(sequence++);
      config = base + ".conf";
      out = base + ".out";
      err = base + ".err";
      trace = base + ".trace";
      if (fixed_port)
        port = fixed_port;
      else {
        auto reservation = listener();
        port = port_of(reservation.get());
      }
      std::ofstream cfg(config);
      cfg << "listen=127.0.0.1:" << port << '\n';
      for (int p : backends) cfg << "backend=127.0.0.1:" << p << '\n';
      cfg.close();
      original = read_file(config);
      child = mode == "product"
                  ? exec_process({product, "--run", config}, out, err)
                  : exec_process({driver, config, trace, mode}, out, err);
      if (!expect_ready) return;
      until(
          [&] {
            return read_file(out).find('\n') != std::string::npos ||
                   !read_file(err).empty();
          },
          "ready stdout");
      if (read_file(out).find('\n') != std::string::npos) return;
      if (read_file(err).find("Address already in use") == std::string::npos)
        throw std::runtime_error("startup: " + read_file(err));
      child.finish();
    }
    throw std::runtime_error("bounded port retries exhausted");
  }

  void stop(int sig = SIGTERM) {
    check(read_file(config) == original, "configuration unchanged");
    check(child.finish(sig) == 0, "service clean exit");
    auto rebound = listener(port);
  }

  void drained() {
    until(
        [&] {
          auto text = read_file(trace);
          auto p = text.rfind("closed ");
          return p != std::string::npos &&
                 text.substr(p).find(" 0 0\n") != std::string::npos;
        },
        "session/token zero");
  }
};

std::string roundtrip(int port, const std::string& payload,
                      std::chrono::milliseconds slow = 0ms) {
  auto fd = client(port);
  std::exception_ptr error;
  std::thread writer([&] {
    try {
      send_all(fd.get(), payload);
      check(shutdown(fd.get(), SHUT_WR) == 0, "client shutdown");
    } catch (...) {
      error = std::current_exception();
      shutdown(fd.get(), SHUT_RDWR);
    }
  });
  std::string reply;
  try {
    if (slow.count()) std::this_thread::sleep_for(slow);
    reply = receive(fd.get());
  } catch (...) {
    shutdown(fd.get(), SHUT_RDWR);
    writer.join();
    throw;
  }
  writer.join();
  if (error) std::rethrow_exception(error);
  return reply;
}

std::size_t occurrences(const std::string& text, const std::string& needle) {
  std::size_t count = 0, p = 0;
  while ((p = text.find(needle, p)) != std::string::npos) {
    ++count;
    p += needle.size();
  }
  return count;
}

std::size_t fd_count(pid_t pid) {
  return std::distance(std::filesystem::directory_iterator(
                           "/proc/" + std::to_string(pid) + "/fd"),
                       std::filesystem::directory_iterator{});
}

void evidence(const std::string& text) { std::cout << text << std::endl; }

void cli_cases() {
  Backend backend;
  {
    Service log_proxy({backend.port});
    const std::string marker = "PAYLOAD_MUST_NOT_APPEAR_582931";
    check(roundtrip(log_proxy.port, marker) == marker,
          "product payload transfer");
    log_proxy.stop();
    check(read_file(log_proxy.out).find(marker) == std::string::npos &&
              read_file(log_proxy.err).find(marker) == std::string::npos,
          "no payload logging");
  }
  for (int sig : {SIGINT, SIGTERM})
    for (bool active : {false, true}) {
      Service proxy({backend.port});
      Fd connection;
      check(read_file(proxy.out) == "TCP 服务已启动：127.0.0.1:" +
                                        std::to_string(proxy.port) + "\n",
            "ready contract");
      if (active) {
        connection = client(proxy.port);
        send_all(connection.get(), "live");
        check(exact(connection.get(), 4) == "live", "active exchange");
      }
      proxy.stop(sig);
      check(read_file(proxy.err).find("TCP 服务已停止") != std::string::npos,
            "stop summary");
    }
  auto occupied = listener();
  Service fail({backend.port}, "product", port_of(occupied.get()), false);
  check(fail.child.finish() == 1 && read_file(fail.out).empty() &&
            read_file(fail.err).find("bind") != std::string::npos,
        "occupied startup");
  std::vector<std::vector<std::string>> cases = {
      {"--run"},
      {"--run", "--help"},
      {"--run", "a", "--check-config", "a"},
      {"--run", "a", "b"},
      {"--run", "--run"}};
  for (auto args : cases) {
    args.insert(args.begin(), product);
    auto name = root + "/cli-" + std::to_string(sequence++);
    auto child = exec_process(args, name + ".out", name + ".err");
    check(child.finish() == 2 && read_file(name + ".out").empty(), "run usage");
  }
  auto name = root + "/invalid";
  std::ofstream(name + ".conf") << "listen=127.0.0.1:0\nbackend=127.0.0.1:2\n";
  auto child = exec_process({product, "--run", name + ".conf"}, name + ".out",
                            name + ".err");
  check(child.finish() == 1 && read_file(name + ".out").empty() &&
            read_file(name + ".err").find("第 1 行") != std::string::npos,
        "run config diagnostic");
  evidence(
      "AC-02 PASS: ready/bind/config/usage; INT+TERM idle+active <1.8s; "
      "rebind");
}

void data_and_rr() {
  Backend a("label", 'A'), b("label", 'B'), c("label", 'C');
  Service proxy({a.port, b.port, c.port}, "observe");
  auto long_client = client(proxy.port);
  check(exact(long_client.get(), 1) == "A", "long initial A");
  auto payload = binary(100003);
  for (char expected : std::string("BCABC"))
    check(roundtrip(proxy.port, payload) == std::string(1, expected) + payload,
          "ABCABC exact");
  send_all(long_client.get(), "still-A");
  check(exact(long_client.get(), 7) == "still-A", "long binding");
  shutdown(long_client.get(), SHUT_WR);
  check(receive(long_client.get()).empty(), "long EOF");
  long_client.reset();
  std::vector<std::thread> clients;
  std::vector<std::exception_ptr> errors(12);
  for (int i = 0; i < 12; ++i)
    clients.emplace_back([&, i] {
      try {
        auto data = binary(180001) + std::to_string(i);
        auto reply = roundtrip(proxy.port, data);
        check(reply.size() == data.size() + 1 && reply.substr(1) == data,
              "concurrent isolation");
      } catch (...) {
        errors[i] = std::current_exception();
      }
    });
  for (auto& thread : clients) thread.join();
  for (auto error : errors)
    if (error) std::rethrow_exception(error);
  proxy.drained();
  auto trace = read_file(proxy.trace);
  check(trace.find("connect-inprogress") != std::string::npos &&
            trace.find("so-error") != std::string::npos,
        "real asynchronous connect evidence");
  proxy.stop();
  evidence(
      "AC-03 PASS: binary NUL exact, 12 concurrent isolated, ABCABC and long "
      "binding; trace=" +
      proxy.trace);
}

void failures_and_timeouts() {
  Backend backend;
  auto unused = listener();
  int refused = port_of(unused.get());
  unused.reset();
  Service proxy({refused, backend.port}, "observe");
  auto bad = client(proxy.port);
  char c;
  auto n = recv(bad.get(), &c, 1, 0);
  check(n <= 0, "refused closed");
  check(roundtrip(proxy.port, "survivor") == "survivor", "failure advances RR");
  proxy.drained();
  auto trace = read_file(proxy.trace);
  check(trace.find("connect-inprogress") != std::string::npos &&
            trace.find("SO_ERROR 111") != std::string::npos,
        "real refusal SO_ERROR errno");
  proxy.stop();
  Service connecting({backend.port}, "connect-timeout");
  auto pending = client(connecting.port);
  send_all(pending.get(), "unread");
  shutdown(pending.get(), SHUT_WR);
  until(
      [&] {
        return read_file(connecting.trace).find("connect-timeout 110") !=
               std::string::npos;
      },
      "connect timeout path");
  connecting.stop();
  Service idle({backend.port}, "idle");
  auto idle_client = client(idle.port);
  send_all(idle_client.get(), "ping");
  check(exact(idle_client.get(), 4) == "ping", "idle establish");
  check(receive(idle_client.get()).empty(), "idle closes");
  // Peer EOF can arrive before the service writes its close observation.
  until(
      [&] {
        return read_file(idle.trace).find("idle-timeout 110") !=
               std::string::npos;
      },
      "idle evidence");
  idle.stop();
  evidence(
      "AC-04 PASS: real EINPROGRESS/event/SO_ERROR=111 and healthy next; "
      "timeout via internal 0ms (unit verifies 5s); traces=" +
      proxy.trace + "," + connecting.trace);
}

void backpressure() {
  for (bool slow_client : {false, true}) {
    Backend backend(slow_client ? "echo" : "slow");
    Service proxy({backend.port}, "small-send");
    auto payload = binary(8 * 1024 * 1024 + 333);
    std::string reply;
    std::exception_ptr error;
    std::thread transfer([&] {
      try {
        reply = roundtrip(proxy.port, payload, slow_client ? 600ms : 0ms);
      } catch (...) {
        error = std::current_exception();
      }
    });
    // 第二会话验证繁忙连接不会独占事件循环。
    try {
      check(roundtrip(proxy.port, "parallel") == "parallel",
            "backpressure other client");
    } catch (...) {
      transfer.join();
      throw;
    }
    transfer.join();
    if (error) std::rethrow_exception(error);
    check(reply == payload, "slow exact payload");
    proxy.drained();
    auto trace = read_file(proxy.trace);
    int direction = slow_client ? 1 : 0;
    bool pause = false, resume = false, blocked = false, suspended = false;
    std::istringstream lines(trace);
    std::string line;
    while (std::getline(lines, line)) {
      std::istringstream fields(line);
      std::string kind;
      unsigned long id, value;
      int side;
      fields >> kind >> id >> side >> value;
      if (kind == "buffer") check(value <= 65536, "bounded buffer");
      if (side == direction) {
        suspended |= kind == "read-suspended";
        pause |= kind == "pause";
        resume |= kind == "resume";
        blocked |= kind == "send-eagain" || kind == "short-send";
      }
    }
    check(pause && resume && blocked && suspended,
          "actual directional backpressure evidence");
    evidence("AC-05 PASS direction=" + std::to_string(direction) +
             " bytes=" + std::to_string(payload.size()) +
             " EAGAIN/short+pause+resume exact trace=" + proxy.trace);
    proxy.stop();
  }
  {
    Backend slow("slow");
    Service stopping({slow.port}, "small-send");
    auto busy = client(stopping.port);
    std::exception_ptr writer_error;
    std::thread writer([&] {
      try {
        send_all(busy.get(), binary(16 * 1024 * 1024));
      } catch (...) {
        writer_error = std::current_exception();
      }
    });
    try {
      until(
          [&] {
            return read_file(stopping.trace).find("read-suspended") !=
                   std::string::npos;
          },
          "busy pause before signal");
      stopping.stop(SIGINT);
    } catch (...) {
      shutdown(busy.get(), SHUT_RDWR);
      writer.join();
      throw;
    }
    writer.join();
    evidence("AC-05 PASS: SIGINT during observed backpressure exited <1.8s");
  }
  Backend backend;
  Service short_write({backend.port}, "short-send");
  auto data = binary(8193);
  check(roundtrip(short_write.port, data) == data,
        "deterministic seven-byte sends");
  short_write.drained();
  check(read_file(short_write.trace).find("short-send") != std::string::npos,
        "short stub observed");
  short_write.stop();
}

void half_close() {
  auto data = binary(256 * 1024 + 17);
  Backend backend("after-eof");
  Service proxy({backend.port}, "observe");
  check(roundtrip(proxy.port, data) == std::string("response\0", 9) + data,
        "request EOF then response exact");
  proxy.drained();
  check(read_file(proxy.trace).find("drained 0") != std::string::npos,
        "normal drained");
  proxy.stop();
  Backend reverse("reverse-fin");
  Service reverse_proxy({reverse.port}, "observe");
  auto fd = client(reverse_proxy.port);
  check(receive(fd.get()) == std::string("reply\0tail", 10),
        "backend first EOF");
  send_all(fd.get(), std::string("after-eof\0data", 14));
  shutdown(fd.get(), SHUT_WR);
  reverse.verify_reverse_fin();
  evidence(
      "AC-06 reverse-fin: parent verified backend exact 14 bytes after EOF");
  reverse_proxy.drained();
  reverse_proxy.stop();
  Backend reset("rst");
  Service reset_proxy({reset.port}, "observe");
  auto reset_client = client(reset_proxy.port);
  char c;
  recv(reset_client.get(), &c, 1, 0);
  reset_proxy.drained();
  auto reset_trace = read_file(reset_proxy.trace);
  check(reset_trace.find("ended") != std::string::npos &&
            reset_trace.find("drained 0") == std::string::npos,
        "RST abnormal");
  reset_proxy.stop();
  Backend echo;
  Service closing({echo.port}, "observe");
  for (int i = 0; i < 20; ++i) {
    auto early = client(closing.port);
    early.reset();
  }
  closing.drained();
  check(roundtrip(closing.port, "alive") == "alive",
        "early frontend close survives");
  closing.stop();
  evidence(
      "AC-06 PASS: both first-FIN directions, data+FIN, RST abnormal, "
      "immediate frontend close");
}

void resources() {
  Backend a("label", 'A'), b("label", 'B');
  Service capped({a.port, b.port}, "capacity");
  auto first = client(capped.port);
  check(exact(first.get(), 1) == "A", "capacity first A");
  auto rejected = client(capped.port);
  check(receive(rejected.get()).empty(), "capacity reject closes");
  shutdown(first.get(), SHUT_WR);
  receive(first.get());
  first.reset();
  capped.drained();
  check(roundtrip(capped.port, "next") == "Bnext",
        "capacity does not consume cursor");
  capped.drained();
  check(read_file(capped.trace).find("capacity-reject") != std::string::npos,
        "capacity evidence");
  capped.stop();
  Backend backend;
  Service loops({backend.port}, "observe");
  check(roundtrip(loops.port, "warm") == "warm", "warmup");
  loops.drained();
  auto baseline = fd_count(loops.child.pid);
  for (int i = 0; i < 220; ++i)
    check(roundtrip(loops.port, std::to_string(i)) == std::to_string(i),
          "220 connections");
  loops.drained();
  auto after = fd_count(loops.child.pid);
  check(after == baseline, "fd returns baseline");
  evidence("AC-07 PASS: 220 loops fd baseline=" + std::to_string(baseline) +
           " after=" + std::to_string(after) +
           " sessions/tokens=0; capacity rejection preserves RR; trace=" +
           loops.trace);
  loops.stop();
  Service backoff({backend.port}, "accept-backoff");
  check(roundtrip(backoff.port, "recovered") == "recovered",
        "resource backoff recovered");
  backoff.drained();
  auto trace = read_file(backoff.trace);
  check(occurrences(trace, "listener-backoff") == 3 &&
            occurrences(trace, "diagnostic") == 1,
        "100ms backoff aggregates diagnostics");
  backoff.stop();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  product = argv[1];
  driver = argv[2];
  root = std::string(argv[3]) + "/run-" + std::to_string(getpid());
  std::filesystem::create_directories(root);
  try {
    cli_cases();
    data_and_rr();
    failures_and_timeouts();
    backpressure();
    half_close();
    resources();
    evidence("S2 TCP integration PASS");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "S2 integration FAIL: " << e.what()
              << " evidence-root=" << root << '\n';
    return 1;
  }
}
