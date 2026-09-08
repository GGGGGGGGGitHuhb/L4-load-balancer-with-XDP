#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
void require(bool ok, const std::string& reason) {
  if (!ok) throw std::runtime_error(reason);
}
std::string read(const std::string& path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), {}};
}
void write(const std::string& path, const std::string& value) {
  std::ofstream out(path);
  out << value;
  out.close();
  require(!out.fail(), "write " + path);
}
struct Fd {
  int value;
  explicit Fd(int fd) : value(fd) { require(fd >= 0, "socket/open"); }
  ~Fd() { close(value); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
};
sockaddr_in address(int port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(port);
  return a;
}
int bind_port(int fd, int port) {
  int yes = 1;
  require(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0,
          "reuseaddr");
  auto a = address(port);
  require(bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0,
          "port rebind " + std::to_string(port));
  socklen_t size = sizeof(a);
  require(getsockname(fd, reinterpret_cast<sockaddr*>(&a), &size) == 0,
          "getsockname");
  return ntohs(a.sin_port);
}
int reserve_port() {
  Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  return bind_port(fd.value, 0);
}
/** Own only our children; even exception paths reap them before reporting. */
struct Child {
  pid_t pid;
  int status = 0;
  bool reaped = false;
  bool product;
  std::string base;
  Child(const std::vector<std::string>& args, std::string path, bool is_product)
      : product(is_product), base(std::move(path)) {
    pid = fork();
    require(pid >= 0, "fork");
    if (pid == 0) {
      int out =
          open((base + ".out").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      int err =
          open((base + ".err").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (out < 0 || err < 0 || dup2(out, 1) < 0 || dup2(err, 2) < 0)
        _exit(125);
      close(out);
      close(err);
      std::vector<char*> argv;
      for (auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
      argv.push_back(nullptr);
      execv(argv[0], argv.data());
      _exit(126);
    }
  }
  ~Child() {
    if (!reaped) {
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
    }
  }
  bool alive() {
    if (reaped) return false;
    pid_t result;
    do {
      result = waitpid(pid, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    require(result >= 0, "waitpid");
    reaped = result == pid;
    return !reaped;
  }
  int code() const {
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  }
  int wait() {
    auto end = Clock::now() + 3s;
    while (alive()) {
      require(Clock::now() < end, "child exit timeout " + base);
      std::this_thread::sleep_for(5ms);
    }
    return code();
  }
  void stop() {
    require(alive(), "unexpected child exit " + base +
                         " code=" + std::to_string(code()));
    require(kill(pid, SIGTERM) == 0, "SIGTERM");
    int result = wait();
    require(product ? result == 0 : result == 128 + SIGTERM,
            "stop status " + base + " code=" + std::to_string(result));
  }
  std::string ready(const std::string& prefix) {
    auto end = Clock::now() + 3s;
    for (;;) {
      require(alive(),
              "early child exit " + base + " code=" + std::to_string(code()));
      auto text = read(base + ".out");
      require(text.size() <= 4096, "oversized ready");
      auto newline = text.find('\n');
      if (newline != std::string::npos) {
        auto line = text.substr(0, newline);
        require(line.starts_with(prefix), "invalid ready " + base);
        return line.substr(prefix.size());
      }
      require(Clock::now() < end, "ready timeout " + base);
      std::this_thread::sleep_for(5ms);
    }
  }
};
struct Run {
  std::string dir, product, echo;
  std::vector<std::unique_ptr<Child>> children;
  std::vector<int> ports;
  std::ostringstream summary;
  int serial = 0;
  Child& start(const std::vector<std::string>& args, const std::string& name,
               bool is_product) {
    auto path = dir + "/" + name + "-" + std::to_string(serial++);
    children.push_back(std::make_unique<Child>(args, path, is_product));
    summary << "child pid=" << children.back()->pid << " log=" << path << '\n';
    return *children.back();
  }
  std::pair<Child*, int> backend(int port = 0, int mask = 0) {
    auto& c = start({echo, std::to_string(port), std::to_string(mask)}, "echo",
                    false);
    auto value = c.ready("echo ready 127.0.0.1:");
    size_t used = 0;
    int actual = std::stoi(value, &used);
    require(used == value.size() && actual > 0 && actual <= 65535 &&
                (port == 0 || actual == port),
            "echo ready port");
    ports.push_back(actual);
    summary << "backend ready port=" << actual << '\n';
    return {&c, actual};
  }
  std::string config(int port, const std::vector<int>& backends) {
    std::string text = "listen=127.0.0.1:" + std::to_string(port) + "\n";
    for (int b : backends)
      text += "backend=127.0.0.1:" + std::to_string(b) + "\n";
    auto path = dir + "/config-" + std::to_string(serial++) + ".conf";
    write(path, text);
    return path;
  }
  std::pair<Child*, int> proxy(const std::vector<int>& backends,
                               bool explicit_fields = false) {
    for (int attempt = 0; attempt < 5; ++attempt) {
      int port = reserve_port();
      auto conf = config(port, backends);
      if (explicit_fields)
        write(conf, "protocol=tcp\nscheduler=round_robin\n" + read(conf));
      auto& check = start({product, "--check-config", conf}, "check", true);
      require(check.wait() == 0, "check-config failed");
      auto& c = start({product, "--run", conf}, "proxy", true);
      try {
        auto value = c.ready("TCP 服务已启动：127.0.0.1:");
        require(value == std::to_string(port), "proxy ready port");
      } catch (...) {
        // Only a diagnosed bind conflict may consume a retry; all other
        // failures surface.
        if (!c.alive() && c.code() == 1 &&
            read(c.base + ".err")
                    .find("bind: " + std::generic_category().message(
                                         EADDRINUSE)) != std::string::npos)
          continue;
        throw;
      }
      ports.push_back(port);
      summary << "proxy ready port=" << port << '\n';
      return {&c, port};
    }
    throw std::runtime_error("proxy bind conflicts exhausted");
  }
  void cleanup() {
    std::string failure;
    for (auto& c : children) {
      if (c->reaped) continue;
      try {
        c->stop();
      } catch (const std::exception& e) {
        failure += std::string(e.what()) + "; ";
        if (!c->reaped) {
          kill(c->pid, SIGKILL);
          try {
            c->wait();
          } catch (...) {
            failure += "reap failed; ";
          }
        }
      }
      summary << "reaped pid=" << c->pid << " code=" << c->code() << '\n';
    }
    for (int port : ports) {
      try {
        Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        bind_port(fd.value, port);
        summary << "rebind OK port=" << port << '\n';
      } catch (const std::exception& e) {
        failure += std::string(e.what()) + "; ";
      }
    }
    require(failure.empty(), "cleanup: " + failure);
  }
};
void wait_io(int fd, short events, Clock::time_point end) {
  require(Clock::now() < end, "I/O timeout");
  pollfd p{fd, events, 0};
  int rc = poll(&p, 1, 10);
  require(rc >= 0 || errno == EINTR, "poll");
}
void connect_to(int fd, int port, Clock::time_point end) {
  auto a = address(port);
  int rc = connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
  require(rc == 0 || errno == EINPROGRESS, "connect");
  if (rc == 0) return;
  for (;;) {
    wait_io(fd, POLLOUT, end);
    pollfd p{fd, POLLOUT, 0};
    if (poll(&p, 1, 0) <= 0) continue;
    int error = 0;
    socklen_t size = sizeof(error);
    require(
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0,
        "connect SO_ERROR");
    return;
  }
}
/** Interleave nonblocking sends and receives, with one deadline and EOF proof.
 */
void exchange(int port, size_t size, int mask = 0) {
  Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  auto end = Clock::now() + 3s;
  connect_to(fd.value, port, end);
  std::string payload(size, '\0');
  for (size_t i = 0; i < size; ++i) payload[i] = static_cast<char>(i % 251);
  std::string received;
  size_t sent = 0;
  bool shut = false;
  for (;;) {
    require(Clock::now() < end, "echo I/O timeout");
    if (sent < payload.size()) {
      auto n = send(fd.value, payload.data() + sent, payload.size() - sent,
                    MSG_NOSIGNAL);
      if (n > 0)
        sent += n;
      else
        require(n < 0 &&
                    (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR),
                "send");
    }
    if (sent == payload.size() && !shut) {
      require(shutdown(fd.value, SHUT_WR) == 0, "SHUT_WR");
      shut = true;
    }
    char bytes[65536];
    auto n = recv(fd.value, bytes, sizeof(bytes), 0);
    if (n == 0) break;
    if (n > 0) {
      received.append(bytes, n);
      require(received.size() <= payload.size(), "excess echo bytes");
    } else
      require(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR,
              "recv");
    wait_io(fd.value, static_cast<short>(POLLIN | (shut ? 0 : POLLOUT)), end);
  }
  for (char& byte : payload) byte ^= mask;
  require(shut && received == payload, "payload mismatch after EOF");
}
void failed_session(int port) {
  Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  auto end = Clock::now() + 3s;
  connect_to(fd.value, port, end);
  for (;;) {
    char byte;
    auto n = recv(fd.value, &byte, 1, 0);
    if (n == 0 || (n < 0 && errno == ECONNRESET)) return;
    require(
        n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR),
        "failed session returned data");
    wait_io(fd.value, POLLIN, end);
  }
}
void stage_scenarios(Run& run) {
  auto [a, ap] = run.backend(0, 0x55);
  auto [b, bp] = run.backend(0, 0xaa);
  for (bool explicit_fields : {false, true}) {
    auto [proxy, port] = run.proxy({ap, bp}, explicit_fields);
    for (int i = 0; i < 4; ++i)
      exchange(port, 8192 + i, i % 2 == 0 ? 0x55 : 0xaa);
    proxy->stop();
    run.summary << "V02 AC01/05 PASS "
                << (explicit_fields ? "explicit" : "default")
                << " TCP A/B/A/B verified by exact XOR binary bytes; restart "
                   "starts A\n";
  }
  // 持有已绑定但未 listen 的 socket，避免被其他进程抢占失败后端端口。
  Fd refused(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  int dead = bind_port(refused.value, 0);
  auto [proxy, port] = run.proxy({dead, bp}, true);
  failed_session(port);
  exchange(port, 16385, 0xaa);
  failed_session(port);
  exchange(port, 16386, 0xaa);
  proxy->stop();
  run.summary << "V02 AC05 PASS failed/B/failed/B: failures return no data; "
                 "next sessions exact B bytes, no retry\n";
  a->stop();
  b->stop();
}
void scenarios(Run& run) {
  auto [a, ap] = run.backend();
  auto [b, bp] = run.backend();
  auto [p, port] = run.proxy({ap, bp});
  for (int i = 0; i < 4; ++i) exchange(port, 1024 + i);
  exchange(port, 1024 * 1024 + 17);
  p->stop();
  std::istringstream logs(read(p->base + ".err"));
  std::vector<int> accepted;
  std::string line;
  while (std::getline(logs, line)) {
    if (line.find("reason=accepted") == std::string::npos) continue;
    auto pos = line.find("backend=127.0.0.1:");
    require(pos != std::string::npos, "backend log");
    accepted.push_back(std::stoi(line.substr(pos + 18)));
  }
  require(accepted == std::vector<int>({ap, bp, ap, bp, ap}),
          "RR A/B/A/B mismatch");
  run.summary << "P1 PASS four exact binary echoes RR=A/B/A/B\nP2 PASS "
                 "bytes=1048593 SHUT_WR EOF exact echo\n";
  a->stop();
  b->stop();
  auto [backend, backend_port] = run.backend();
  auto [single, single_port] = run.proxy({backend_port});
  exchange(single_port, 4096);
  backend->stop();
  {
    Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    bind_port(fd.value, backend_port);
  }
  failed_session(single_port);
  require(single->alive(), "proxy died after backend stop");
  auto [restored, restored_port] = run.backend(backend_port);
  exchange(single_port, 4097);
  single->stop();
  restored->stop();
  require(read(single->base + ".err")
                  .find("errno=" + std::to_string(ECONNREFUSED)) !=
              std::string::npos,
          "missing backend connect refusal evidence");
  run.summary << "P3 PASS backend reaped, no listener, failed new session, "
                 "proxy alive, same-port restore exact echo port="
              << restored_port << '\n';
  auto bad = run.dir + "/bad.conf";
  write(bad, "listen=broken\n");
  auto& invalid = run.start({run.product, "--run", bad}, "invalid", true);
  require(invalid.wait() == 1 &&
              read(invalid.base + ".out").find("TCP 服务已启动") ==
                  std::string::npos &&
              !read(invalid.base + ".err").empty(),
          "invalid config outcome");
  Fd occupied(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  int used = bind_port(occupied.value, 0);
  require(listen(occupied.value, 1) == 0, "occupied listen");
  auto conf = run.config(used, {backend_port});
  auto& conflict = run.start({run.product, "--run", conf}, "occupied", true);
  require(conflict.wait() == 1 && read(conflict.base + ".out").empty() &&
              read(conflict.base + ".err")
                      .find("bind: " + std::generic_category().message(
                                           EADDRINUSE)) != std::string::npos,
          "occupied port outcome");
  run.summary << "P4 PASS bad config exit=1 no ready; occupied port exit=1 "
                 "EADDRINUSE\n";
}
}  // namespace
int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "用法: tcp_product_smoke <l4lb> <tcp_echo_backend> <证据根>\n";
    return 2;
  }
  Run run;
  try {
    run.product = std::filesystem::absolute(argv[1]).string();
    run.echo = std::filesystem::absolute(argv[2]).string();
    std::filesystem::create_directories(argv[3]);
    std::string pattern =
        (std::filesystem::absolute(argv[3]) / "run-XXXXXX").string();
    char* dir = mkdtemp(pattern.data());
    require(dir != nullptr, "mkdtemp");
    run.dir = dir;
    std::string failure;
    try {
      scenarios(run);
      stage_scenarios(run);
    } catch (const std::exception& e) {
      failure = e.what();
    }
    try {
      run.cleanup();
    } catch (const std::exception& e) {
      failure += std::string(" ") + e.what();
    }
    run.summary << (failure.empty() ? "PASS" : "FAIL " + failure) << '\n';
    write(run.dir + "/result.txt", run.summary.str());
    require(failure.empty(), failure);
    std::cout << "PASS 产品 TCP 验收，证据：" << run.dir << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL 产品 TCP 验收：" << e.what() << "；证据：" << run.dir
              << '\n';
    return 1;
  }
}
