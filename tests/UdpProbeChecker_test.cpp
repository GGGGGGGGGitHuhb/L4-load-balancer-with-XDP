#include "health/UdpProbeChecker.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {
bool failRandomSource = false;
}

// Interpose only inside this test executable; production has no fault flag.
extern "C" ssize_t getrandom(void* buffer, size_t size, unsigned flags) {
  if (failRandomSource) {
    errno = ENOSYS;
    return -1;
  }
  return syscall(SYS_getrandom, buffer, size, flags);
}

namespace {
using namespace std::chrono_literals;
using namespace l4lb::health;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

ProbeClock::time_point at(int milliseconds) {
  return ProbeClock::time_point{} + std::chrono::milliseconds(milliseconds);
}

ProbeTarget target(const std::string& id = "blue") {
  return {id,
          "lo",
          1,
          {2, 0, 0, 0, 0, 1},
          {2, 0, 0, 0, 0, 2},
          htonl(0x7f000001),
          htons(9001)};
}

struct Reply {
  std::vector<uint8_t> bytes;
  int error = 0;
};

struct FakeSocket {
  std::vector<ProbePacket> requests;
  std::deque<Reply> replies;
  int sendError = 0;
  bool partialSend = false;
};

class FakeTransport final : public ProbeTransport {
 public:
  std::map<int, FakeSocket> sockets;
  int openCalls = 0;
  int closeCalls = 0;
  int failOpen = -1;

  int openProbeSocket(const ProbeTarget&) override {
    if (openCalls++ == failOpen) throw std::runtime_error("local_error open");
    int handle = 10;
    while (sockets.contains(handle)) ++handle;
    sockets.emplace(handle, FakeSocket{});
    return handle;
  }

  void closeProbeSocket(int handle) noexcept override {
    ++closeCalls;
    sockets.erase(handle);
  }

  ProbeIoResult sendProbePacket(int handle,
                                std::span<const uint8_t> packet) override {
    auto& socket = sockets.at(handle);
    if (socket.sendError) return {-1, socket.sendError};
    if (socket.partialSend) return {1, 0};
    ProbePacket request{};
    std::copy(packet.begin(), packet.end(), request.begin());
    socket.requests.push_back(request);
    return {static_cast<int>(packet.size()), 0};
  }

  ProbeIoResult receiveProbePacket(int handle,
                                   std::span<uint8_t> buffer) override {
    auto& replies = sockets.at(handle).replies;
    if (replies.empty()) return {-1, EAGAIN};
    auto reply = std::move(replies.front());
    replies.pop_front();
    if (reply.error) return {-1, reply.error};
    std::copy_n(reply.bytes.begin(),
                std::min(reply.bytes.size(), buffer.size()), buffer.begin());
    return {static_cast<int>(reply.bytes.size()), 0};
  }

  void echo(int handle = 10) {
    const auto& packet = sockets.at(handle).requests.back();
    sockets.at(handle).replies.push_back({{packet.begin(), packet.end()}, 0});
  }
};

class LogicalCompletionClock final : public ProbeCompletionClock {
 public:
  std::chrono::milliseconds delay{0};

  ProbeClock::time_point nowAtCompletion(
      ProbeClock::time_point tickTime) override {
    return tickTime + delay;
  }
};

std::shared_ptr<LogicalCompletionClock> logicalClock() {
  return std::make_shared<LogicalCompletionClock>();
}

std::shared_ptr<ProbeNonceSequence> nonce() {
  return std::make_shared<ProbeNonceSequence>(0x0102030405060708ULL);
}

void testNonce() {
  ProbeNonceSequence source(0x0102030405060708ULL, 0x1112131415161717ULL);
  auto packet = source.generateProbePacket();
  require(std::memcmp(packet.data(), "L4LBHC01", 8) == 0, "wrong magic");
  for (unsigned index = 0; index < 8; ++index) {
    require(packet[8 + index] == index + 1, "epoch is not network order");
    require(packet[16 + index] == index + 0x11,
            "sequence is not network order");
  }
  ProbeNonceSequence exhausted(0, std::numeric_limits<uint64_t>::max());
  bool rejected = false;
  try {
    exhausted.generateProbePacket();
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  require(rejected, "sequence wrapped");
}

void testTransitions() {
  auto transport = std::make_shared<FakeTransport>();
  UdpProbeChecker checker({target()}, {}, transport, nonce(), logicalClock());
  require(checker.copyHealthyProbeTargets().empty() &&
              checker.inFlightProbeCount() == 0,
          "unknown is selectable");
  require(checker.nextProbeWakeup() == at(0), "first probe not immediate");
  checker.pollProbeTransitions(at(0));
  require(
      checker.inFlightProbeCount() == 1 && checker.nextProbeWakeup() == at(500),
      "deadline");
  transport->echo();
  require(checker.pollProbeTransitions(at(1)).empty(),
          "one success became healthy");
  checker.pollProbeTransitions(at(1000));
  transport->echo();
  auto changes = checker.pollProbeTransitions(at(1001));
  require(changes.size() == 1 && changes[0].to == ProbeState::kHealthy,
          "two successes missing");
  for (int cycle = 2; cycle < 5; ++cycle) {
    checker.pollProbeTransitions(at(cycle * 1000));
    changes = checker.pollProbeTransitions(at(cycle * 1000 + 500));
  }
  require(changes.size() == 1 && changes[0].to == ProbeState::kUnhealthy &&
              changes[0].reason == "timeout",
          "three failures missing");
  require(checker.copyHealthyProbeTargets().empty(), "unhealthy selectable");
  for (int cycle = 5; cycle < 7; ++cycle) {
    checker.pollProbeTransitions(at(cycle * 1000));
    transport->echo();
    changes = checker.pollProbeTransitions(at(cycle * 1000 + 1));
  }
  require(changes.size() == 1 && changes[0].to == ProbeState::kHealthy,
          "recovery missing");
  auto snapshot = checker.copyProbeSnapshots()[0];
  require(
      snapshot.consecutiveSuccesses == 2 && snapshot.consecutiveFailures == 0,
      "counter reset");
  checker.pollProbeTransitions(at(7000));
  transport->echo();
  checker.pollProbeTransitions(at(7001));
  require(checker.copyProbeSnapshots()[0].consecutiveSuccesses == 2,
          "counter did not saturate");
}

void testReplyBoundaries() {
  auto transport = std::make_shared<FakeTransport>();
  UdpProbeChecker checker({target()}, {}, transport, nonce(), logicalClock());
  checker.pollProbeTransitions(at(0));
  auto packet = transport->sockets.at(10).requests.back();
  auto& queue = transport->sockets.at(10).replies;
  queue.push_back({std::vector<uint8_t>(23, 0), 0});
  queue.push_back({std::vector<uint8_t>(25, 0), 0});
  auto wrong = packet;
  ++wrong[23];
  queue.push_back({{wrong.begin(), wrong.end()}, 0});
  checker.pollProbeTransitions(at(499));
  require(
      checker.inFlightProbeCount() == 1 && checker.lastReceiveEventCount() == 3,
      "bad replies completed probe");
  transport->echo();
  checker.pollProbeTransitions(at(500));
  require(checker.copyProbeSnapshots()[0].consecutiveFailures == 1 &&
              checker.copyProbeSnapshots()[0].consecutiveSuccesses == 0,
          "exact deadline accepted reply");
  checker.pollProbeTransitions(at(1000));
  require(checker.inFlightProbeCount() == 1 &&
              checker.copyProbeSnapshots()[0].consecutiveFailures == 1,
          "late old token accepted");
  transport->echo();
  transport->echo();
  checker.pollProbeTransitions(at(1001));
  checker.pollProbeTransitions(at(2000));
  require(checker.inFlightProbeCount() == 1 &&
              checker.copyProbeSnapshots()[0].consecutiveSuccesses == 1,
          "duplicate became next success");
  // A delayed controller sends one fresh probe, not thousands of catch-up
  // probes.
  checker.pollProbeTransitions(at(100000));
  require(transport->sockets.at(10).requests.size() == 4, "catch-up burst");
  require(checker.nextProbeWakeup() == at(100500),
          "new deadline based on stale time");
}

void testCompletionDeadline() {
  auto transport = std::make_shared<FakeTransport>();
  auto clock = logicalClock();
  UdpProbeChecker checker({target()}, {}, transport, nonce(), clock);
  checker.pollProbeTransitions(at(0));
  transport->echo();
  clock->delay = 1ms;
  checker.pollProbeTransitions(at(499));
  require(checker.copyProbeSnapshots()[0].consecutiveFailures == 1 &&
              checker.copyProbeSnapshots()[0].consecutiveSuccesses == 0,
          "reply processing crossed deadline but counted success");
}

void testBudget() {
  auto transport = std::make_shared<FakeTransport>();
  UdpProbeChecker checker({target("blue"), target("green")}, {}, transport,
                          nonce(), logicalClock());
  checker.pollProbeTransitions(at(0));
  for (int index = 0; index < 600; ++index)
    transport->sockets.at(10).replies.push_back(
        {std::vector<uint8_t>(24, 0), 0});
  transport->echo(11);
  checker.pollProbeTransitions(at(1));
  require(checker.lastReceiveEventCount() == 256,
          "receive budget not enforced");
  checker.pollProbeTransitions(at(2));
  require(checker.lastReceiveEventCount() <= 256 &&
              checker.copyProbeSnapshots()[1].consecutiveSuccesses == 1,
          "noisy peer starves other target");
}

void testReload() {
  auto transport = std::make_shared<FakeTransport>();
  auto source = nonce();
  auto old = std::make_unique<UdpProbeChecker>(
      std::vector{target()}, std::vector<ProbeSnapshot>{}, transport, source,
      logicalClock());
  old->pollProbeTransitions(at(0));
  transport->echo();
  old->pollProbeTransitions(at(1));
  old->pollProbeTransitions(at(1000));
  auto oldPacket = transport->sockets.at(10).requests.back();
  auto candidate = std::make_unique<UdpProbeChecker>(
      std::vector{target("green"), target()}, old->copyProbeSnapshots(),
      transport, source, logicalClock());
  require(
      candidate->inFlightProbeCount() == 0 && old->inFlightProbeCount() == 1,
      "candidate mutates old probe");
  require(candidate->copyProbeSnapshots()[1].consecutiveSuccesses == 1,
          "completed health not preserved");
  old.reset();
  candidate->pollProbeTransitions(at(1001));
  // Deliver an old token to a new socket despite fd reuse: still no success.
  transport->sockets.at(12).replies.push_back(
      {{oldPacket.begin(), oldPacket.end()}, 0});
  candidate->pollProbeTransitions(at(1002));
  require(candidate->copyProbeSnapshots()[1].consecutiveSuccesses == 1 &&
              candidate->inFlightProbeCount() == 2,
          "old token accepted after reorder");
  transport->echo(12);
  candidate->pollProbeTransitions(at(1003));
  require(candidate->copyHealthyProbeTargets().size() == 1 &&
              candidate->copyHealthyProbeTargets()[0].id == "blue",
          "preserved counter missing");
  auto snapshots = candidate->copyProbeSnapshots();
  for (int field = 0; field < 6; ++field) {
    auto changed = target();
    if (field == 0) changed.id = "renamed";
    if (field == 1) ++changed.ifindex;
    if (field == 2) ++changed.sourceMac[5];
    if (field == 3) ++changed.destinationMac[5];
    if (field == 4) ++changed.probeAddress;
    if (field == 5) ++changed.probePort;
    UdpProbeChecker reset({changed}, snapshots, transport, source,
                          logicalClock());
    require(reset.copyProbeSnapshots()[0].state == ProbeState::kUnknown &&
                reset.copyProbeSnapshots()[0].consecutiveSuccesses == 0,
            "changed identity kept health");
  }
  candidate.reset();
  require(transport->sockets.empty(), "reload leaked sockets");
  UdpProbeChecker reused({target()}, {}, transport, source, logicalClock());
  reused.pollProbeTransitions(at(2000));
  transport->sockets.at(10).replies.push_back(
      {{oldPacket.begin(), oldPacket.end()}, 0});
  reused.pollProbeTransitions(at(2001));
  require(reused.inFlightProbeCount() == 1, "reused fd accepted old identity");
}

void testErrorsAndBounds() {
  for (int error : {ENOBUFS, ENOMEM, EAGAIN, ECONNREFUSED, ENETUNREACH}) {
    auto transport = std::make_shared<FakeTransport>();
    UdpProbeChecker checker({target()}, {}, transport, nonce(), logicalClock());
    transport->sockets.at(10).sendError = error;
    std::vector<ProbeTransition> changes;
    for (int cycle = 0; cycle < 3; ++cycle)
      changes = checker.pollProbeTransitions(at(cycle * 1000));
    require(changes.size() == 1 && changes[0].to == ProbeState::kUnhealthy,
            "send errors not failed");
    bool local = error == ENOBUFS || error == ENOMEM || error == EAGAIN;
    require(changes[0].reason == (local ? "local_error" : "target_error"),
            "error attribution");
  }
  auto transport = std::make_shared<FakeTransport>();
  std::vector<ProbeTarget> targets;
  for (int index = 0; index < 64; ++index)
    targets.push_back(target(std::to_string(index)));
  {
    UdpProbeChecker checker(targets, {}, transport, nonce(), logicalClock());
    checker.pollProbeTransitions(at(0));
    require(checker.inFlightProbeCount() == 64, "64 target bound");
    bool rejected = false;
    try {
      checker.pollProbeTransitions(at(-1));
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    require(rejected && transport->sockets.empty(),
            "clock invariant did not clean up");
  }
  targets.push_back(target("65"));
  bool rejected = false;
  try {
    UdpProbeChecker checker(targets, {}, transport, nonce(), logicalClock());
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected && transport->sockets.empty(), "65 targets accepted");
  transport->failOpen = transport->openCalls + 1;
  rejected = false;
  try {
    UdpProbeChecker checker({target(), target("green")}, {}, transport, nonce(),
                            logicalClock());
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  require(rejected && transport->sockets.empty(),
          "candidate open failure leaked first socket");
  transport->failOpen = -1;
  {
    UdpProbeChecker checker({target()}, {}, transport, nonce(), logicalClock());
    transport->sockets.at(10).sendError = EBADF;
    rejected = false;
    try {
      checker.pollProbeTransitions(at(0));
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    require(rejected && transport->sockets.empty(),
            "internal socket error not fatal");
  }
  {
    auto exhausted = std::make_shared<ProbeNonceSequence>(
        1, std::numeric_limits<uint64_t>::max());
    UdpProbeChecker checker({target()}, {}, transport, exhausted,
                            logicalClock());
    rejected = false;
    try {
      checker.pollProbeTransitions(at(0));
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    require(rejected && transport->sockets.empty(),
            "nonce exhaustion did not terminate");
  }
  {
    UdpProbeChecker checker({}, {}, transport, nonce(), logicalClock());
    require(checker.pollProbeTransitions(at(0)).empty() &&
                checker.nextProbeWakeup() == ProbeClock::time_point::max(),
            "empty set");
  }
}

void testCandidateAbortAndRename() {
  auto transport = std::make_shared<FakeTransport>();
  auto source = nonce();
  UdpProbeChecker original({target()}, {}, transport, source, logicalClock());
  original.pollProbeTransitions(at(0));
  transport->echo();
  original.pollProbeTransitions(at(1));
  original.pollProbeTransitions(at(1000));
  transport->echo();
  original.pollProbeTransitions(at(1001));
  original.pollProbeTransitions(at(2000));
  {
    UdpProbeChecker candidate({target("green")}, original.copyProbeSnapshots(),
                              transport, source, logicalClock());
    require(candidate.inFlightProbeCount() == 0,
            "unpublished candidate sent probe");
  }
  require(original.inFlightProbeCount() == 1 &&
              original.copyHealthyProbeTargets().size() == 1 &&
              transport->sockets.size() == 1,
          "aborted candidate changed original");
  auto renamed = target();
  renamed.interfaceName = "renamed0";
  UdpProbeChecker candidate({renamed}, original.copyProbeSnapshots(), transport,
                            source, logicalClock());
  require(candidate.copyProbeSnapshots()[0].state == ProbeState::kHealthy,
          "same ifindex with renamed interface lost health");
}

void testReceiveErrors() {
  for (int error : {ENOBUFS, ENOMEM, ECONNREFUSED, EHOSTUNREACH}) {
    auto transport = std::make_shared<FakeTransport>();
    UdpProbeChecker checker({target()}, {}, transport, nonce(), logicalClock());
    std::vector<ProbeTransition> changes;
    for (int cycle = 0; cycle < 3; ++cycle) {
      checker.pollProbeTransitions(at(cycle * 1000));
      transport->sockets.at(10).replies.push_back({{}, error});
      changes = checker.pollProbeTransitions(at(cycle * 1000 + 1));
    }
    require(changes.size() == 1 && changes[0].to == ProbeState::kUnhealthy,
            "receive errors not failed");
    bool local = error == ENOBUFS || error == ENOMEM;
    require(changes[0].reason == (local ? "local_error" : "target_error"),
            "receive error attribution");
  }
  auto transport = std::make_shared<FakeTransport>();
  UdpProbeChecker checker({target()}, {}, transport, nonce(), logicalClock());
  checker.pollProbeTransitions(at(0));
  transport->sockets.at(10).replies.push_back({{}, EINTR});
  transport->echo();
  checker.pollProbeTransitions(at(1));
  require(checker.copyProbeSnapshots()[0].consecutiveSuccesses == 1 &&
              checker.lastReceiveEventCount() == 2,
          "interrupted receive not bounded/retried");
  checker.pollProbeTransitions(at(1000));
  transport->sockets.at(10).replies.push_back({{}, EBADF});
  bool rejected = false;
  try {
    checker.pollProbeTransitions(at(1001));
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  require(rejected && transport->sockets.empty(),
          "receive invariant failure leaked sockets");
}

struct Socket {
  int fd = -1;

  ~Socket() {
    if (fd >= 0) ::close(fd);
  }
};

int waitReadable(int fd) {
  pollfd item{fd, POLLIN, 0};
  return poll(&item, 1, 200);
}

void testRealSocket() {
  Socket server{socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
  Socket otherPeer{
      socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
  require(server.fd >= 0 && otherPeer.fd >= 0, "UDP test socket");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(bind(server.fd, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) == 0,
          "UDP test bind");
  socklen_t addressLength = sizeof(address);
  require(getsockname(server.fd, reinterpret_cast<sockaddr*>(&address),
                      &addressLength) == 0,
          "UDP test address");
  auto endpoint = target();
  endpoint.ifindex = if_nametoindex("lo");
  endpoint.probePort = address.sin_port;
  UdpProbeChecker checker({endpoint}, {}, {}, {}, logicalClock());
  ProbePacket previous{};
  for (int cycle = 0; cycle < 2; ++cycle) {
    checker.pollProbeTransitions(at(cycle * 1000));
    require(waitReadable(server.fd) == 1, "bound UDP request absent");
    ProbePacket packet{};
    sockaddr_in client{};
    socklen_t clientLength = sizeof(client);
    require(recvfrom(server.fd, packet.data(), packet.size(), MSG_TRUNC,
                     reinterpret_cast<sockaddr*>(&client), &clientLength) == 24,
            "UDP request size");
    require(
        std::memcmp(packet.data(), "L4LBHC01", 8) == 0 && packet != previous,
        "UDP request nonce");
    previous = packet;
    // A correct token from a different UDP peer must be discarded by connect.
    require(sendto(otherPeer.fd, packet.data(), packet.size(), 0,
                   reinterpret_cast<sockaddr*>(&client), clientLength) == 24,
            "other-peer send");
    checker.pollProbeTransitions(at(cycle * 1000 + 1));
    require(checker.inFlightProbeCount() == 1, "other peer accepted");
    auto wrong = packet;
    ++wrong[23];
    sendto(server.fd, wrong.data(), wrong.size(), 0,
           reinterpret_cast<sockaddr*>(&client), clientLength);
    sendto(server.fd, packet.data(), 23, 0,
           reinterpret_cast<sockaddr*>(&client), clientLength);
    std::array<uint8_t, 25> longReply{};
    std::copy(packet.begin(), packet.end(), longReply.begin());
    sendto(server.fd, longReply.data(), longReply.size(), 0,
           reinterpret_cast<sockaddr*>(&client), clientLength);
    checker.pollProbeTransitions(at(cycle * 1000 + 2));
    require(checker.inFlightProbeCount() == 1,
            "real wrong/short/long echo accepted");
    require(sendto(server.fd, packet.data(), packet.size(), 0,
                   reinterpret_cast<sockaddr*>(&client), clientLength) == 24,
            "echo response send");
    checker.pollProbeTransitions(at(cycle * 1000 + 3));
    require(checker.inFlightProbeCount() == 0,
            "matching real echo not accepted");
  }
  require(checker.copyHealthyProbeTargets().size() == 1,
          "real echo did not become healthy");
  {
    UdpProbeChecker wallClockChecker({endpoint});
    for (int cycle = 0; cycle < 2; ++cycle) {
      if (cycle)
        require(poll(nullptr, 0, 1010) == 0, "wall-clock interval wait");
      wallClockChecker.pollProbeTransitions(ProbeClock::now());
      require(waitReadable(server.fd) == 1, "production clock request absent");
      ProbePacket packet{};
      sockaddr_in client{};
      socklen_t clientLength = sizeof(client);
      require(
          recvfrom(server.fd, packet.data(), packet.size(), MSG_TRUNC,
                   reinterpret_cast<sockaddr*>(&client), &clientLength) == 24,
          "production request size");
      require(std::equal(packet.begin() + 8, packet.begin() + 16,
                         previous.begin() + 8) &&
                  std::lexicographical_compare(
                      previous.begin() + 16, previous.end(),
                      packet.begin() + 16, packet.end()),
              "default process epoch/sequence not shared across checkers");
      previous = packet;
      require(sendto(server.fd, packet.data(), packet.size(), 0,
                     reinterpret_cast<sockaddr*>(&client), clientLength) == 24,
              "production echo send");
      wallClockChecker.pollProbeTransitions(ProbeClock::now());
    }
    require(wallClockChecker.copyHealthyProbeTargets().size() == 1,
            "production completion clock rejected timely echoes");
  }
  for (int cycle = 2; cycle < 5; ++cycle) {
    checker.pollProbeTransitions(at(cycle * 1000));
    checker.pollProbeTransitions(at(cycle * 1000 + 500));
  }
  require(checker.copyHealthyProbeTargets().empty(),
          "silent UDP remained healthy");
  // TCP accepting on a port cannot make its UDP probe healthy.
  Socket tcp{socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
  address.sin_port = 0;
  require(tcp.fd >= 0 &&
              bind(tcp.fd, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == 0 &&
              listen(tcp.fd, 1) == 0,
          "TCP-only fixture");
  addressLength = sizeof(address);
  getsockname(tcp.fd, reinterpret_cast<sockaddr*>(&address), &addressLength);
  endpoint.probePort = address.sin_port;
  UdpProbeChecker tcpOnly({endpoint}, {}, {}, {}, logicalClock());
  for (int cycle = 0; cycle < 3; ++cycle) {
    tcpOnly.pollProbeTransitions(at(cycle * 1000));
    tcpOnly.pollProbeTransitions(at(cycle * 1000 + 500));
  }
  require(tcpOnly.copyProbeSnapshots()[0].state == ProbeState::kUnhealthy,
          "TCP-only mistaken for UDP health");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--random-failure") {
      failRandomSource = true;
      auto transport = std::make_shared<FakeTransport>();
      bool rejected = false;
      try {
        UdpProbeChecker checker({target()}, {}, transport);
      } catch (const std::system_error&) {
        rejected = true;
      }
      require(rejected && transport->openCalls == 0,
              "random initialization failure did not abort before sockets");
      std::cout
          << "UDP probe production random-source initialization failure PASS\n";
    } else if (argc == 2 && std::string(argv[1]) == "--socket") {
      testRealSocket();
      std::cout << "UDP probe real bound socket, peer filter, malformed echo, "
                   "silence and TCP-only PASS\n";
    } else {
      require(argc == 1,
              "usage: UdpProbeChecker_test [--socket|--random-failure]");
      testNonce();
      testTransitions();
      testReplyBoundaries();
      testBudget();
      testCompletionDeadline();
      testReload();
      testErrorsAndBounds();
      testCandidateAbortAndRename();
      testReceiveErrors();
      std::cout << "UDP probe deterministic "
                   "nonce/time/state/reload/errors/budget PASS\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
