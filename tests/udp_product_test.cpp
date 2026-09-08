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
  auto a = address(port);
  require(bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0,
          "port rebind " + std::to_string(port));
  socklen_t size = sizeof(a);
  require(getsockname(fd, reinterpret_cast<sockaddr*>(&a), &size) == 0,
          "getsockname");
  return ntohs(a.sin_port);
}
int reserve_port() {
  Fd fd(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
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
struct Packet {
  std::string bytes;
  sockaddr_in from{};
};
Packet receive_packet(int fd) {
  pollfd event{fd, POLLIN, 0};
  require(poll(&event, 1, 700) == 1 && (event.revents & POLLIN),
          "UDP packet deadline");
  Packet p;
  std::string bytes(65536, '\0');
  socklen_t size = sizeof(p.from);
  auto n = recvfrom(fd, bytes.data(), bytes.size(), 0,
                    reinterpret_cast<sockaddr*>(&p.from), &size);
  require(n >= 0, "UDP receive");
  bytes.resize(n);
  p.bytes = std::move(bytes);
  return p;
}
void send_packet(int fd, const sockaddr_in& to, const std::string& value) {
  require(sendto(fd, value.data(), value.size(), 0,
                 reinterpret_cast<const sockaddr*>(&to),
                 sizeof(to)) == static_cast<ssize_t>(value.size()),
          "UDP test send");
}
struct ProductRun {
  std::string program, dir, mutation;
  std::vector<std::unique_ptr<Child>> children;
  std::ostringstream summary;
  int serial = 0;
  Child& start(std::vector<std::string> args) {
    auto path = dir + "/child-" + std::to_string(serial++);
    children.push_back(std::make_unique<Child>(args, path, true));
    summary << "child pid=" << children.back()->pid << " path=" << path << '\n';
    return *children.back();
  }
  std::string config(int port, int a, int b) {
    auto file = dir + "/config-" + std::to_string(serial++) + ".conf";
    write(file, "protocol=udp\nscheduler=round_robin\nlisten=127.0.0.1:" +
                    std::to_string(port) +
                    "\nbackend=127.0.0.1:" + std::to_string(a) +
                    "\nbackend=127.0.0.1:" + std::to_string(b) + "\n");
    return file;
  }
  void cleanup() {
    std::string errors;
    for (auto& child : children) {
      if (!child->reaped) try {
          child->stop();
        } catch (const std::exception& e) {
          errors += std::string(e.what()) + ";";
        }
      if (child->reaped)
        summary << "reaped pid=" << child->pid << " code=" << child->code()
                << '\n';
    }
    require(errors.empty(), "cleanup " + errors);
  }
  void scenarios() {
    Fd a(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)),
        b(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    int ap = bind_port(a.value, 0), bp = bind_port(b.value, 0);
    int port = reserve_port();
    auto conf = config(port, ap, bp);
    auto& check = start({program, "--check-config", conf});
    require(check.wait() == 0 &&
                read(check.base + ".out") == "配置有效：UDP，后端数量=2\n" &&
                read(check.base + ".err").empty(),
            "UDP check exact");
    pollfd probe{a.value, POLLIN, 0};
    require(poll(&probe, 1, 0) == 0, "check touched backend");
    auto& proxy = start({program, "--run", conf});
    require(proxy.ready("UDP 服务已启动：127.0.0.1:") == std::to_string(port),
            "UDP ready exact");
    Fd c(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)),
        d(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)),
        e(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    bind_port(c.value, 0);
    bind_port(d.value, 0);
    bind_port(e.value, 0);
    auto exchange = [&](int client, int backend, std::size_t size, int mask) {
      std::string value(size, '\0');
      for (std::size_t i = 0; i < size; ++i)
        value[i] = static_cast<char>((i + size) % 251);
      summary << "reached packet length=" << size
              << " backend=" << (backend == a.value ? 'A' : 'B')
              << " mutation=" << mutation << '\n';
      send_packet(client, address(port), value);
      auto request = receive_packet(backend);
      require(request.bytes == value, "request exact payload");
      auto response = value;
      for (auto& ch : response) ch ^= mask;
      if (mutation == "payload" && !response.empty()) response[0] ^= 1;
      // 测试故障模型：错误地把收到的零长报文当 EOF，丢弃回复。
      if (!(mutation == "zero-eof" && size == 0))
        send_packet(backend, request.from, response);
      auto got = receive_packet(client);
      auto expected = value;
      for (auto& ch : expected) ch ^= mask;
      require(got.bytes == expected, "reply exact payload");
      require(got.from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
                  ntohs(got.from.sin_port) == port,
              "UDP reply source");
    };
    exchange(c.value, mutation == "selection" ? b.value : a.value, 257, 0x55);
    exchange(c.value, a.value, 0, 0x55);
    exchange(c.value, a.value, 1, 0x55);
    exchange(d.value, b.value, 65507, 0xaa);
    exchange(e.value, a.value, 513, 0x55);
    summary << "AC01/02/04 PRODUCT exact A/A/A/B/A, lengths257/0/1/65507/513 "
               "PASS\n";
    require(kill(proxy.pid, SIGINT) == 0 && proxy.wait() == 0,
            "SIGINT clean exit");
    require(
        read(proxy.base + ".err")
                .find("UDP 服务已停止：全部 flow "
                      "已关闭（不保证在途数据排空）\n") != std::string::npos,
        "stop exact text");
    {
      Fd occupied(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
      bind_port(occupied.value, port);
      auto& failed = start({program, "--run", conf});
      require(
          failed.wait() == 1 && read(failed.base + ".out").empty() &&
              read(failed.base + ".err").find("UDP bind") != std::string::npos,
          "occupied UDP bind fails no ready");
    }
    auto& term = start({program, "--run", conf});
    term.ready("UDP 服务已启动：127.0.0.1:");
    term.stop();
    Fd rebind(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    bind_port(rebind.value, port);
    summary << "AC01 PRODUCT SIGINT/SIGTERM exit0, occupied bind exit1 no "
               "ready, listener rebind PASS\n";
  }
};
}  // namespace
int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) return 2;
  ProductRun run;
  try {
    run.program = std::filesystem::absolute(argv[1]).string();
    if (argc == 4) run.mutation = argv[3];
    std::filesystem::create_directories(argv[2]);
    auto pattern = (std::filesystem::absolute(argv[2]) / "run-XXXXXX").string();
    auto path = mkdtemp(pattern.data());
    require(path != nullptr, "mkdtemp");
    run.dir = path;
    std::string failure;
    try {
      run.scenarios();
    } catch (const std::exception& e) {
      failure = e.what();
    }
    try {
      run.cleanup();
    } catch (const std::exception& e) {
      failure += " cleanup: " + std::string(e.what());
    }
    run.summary << (failure.empty() ? "PASS" : "FAIL " + failure) << '\n';
    write(run.dir + "/result.txt", run.summary.str());
    require(failure.empty(), failure);
    std::cout << "PASS UDP product evidence=" << run.dir << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL UDP product " << e.what() << " evidence=" << run.dir
              << '\n';
    return 1;
  }
}
