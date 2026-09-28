#include "control/RuntimeDsrConfig.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using l4lb::control::parseRuntimeConfigText;
using l4lb::control::readRuntimeConfigFile;

namespace {
unsigned checks = 0;

void require(bool condition, const char* reason) {
  ++checks;
  if (!condition) throw std::runtime_error(reason);
}

void reject(const std::string& text) {
  try {
    parseRuntimeConfigText(text);
  } catch (const std::invalid_argument&) {
    ++checks;
    return;
  }
  throw std::runtime_error("accepted malformed runtime configuration");
}

void rejectFile(const std::filesystem::path& path) {
  try {
    readRuntimeConfigFile(path.string());
  } catch (const std::invalid_argument&) {
    ++checks;
    return;
  }
  throw std::runtime_error("accepted unsafe configuration path");
}

void fileCases(const std::filesystem::path& root) {
  auto path = root / "configuration";
  {
    std::ofstream file(path);
    file << "schema=1\n";
  }
  require(readRuntimeConfigFile(path.string()) == "schema=1\n", "bounded file");
  std::filesystem::create_symlink(path, root / "symlink");
  rejectFile(root / "symlink");
  rejectFile(root);
  rejectFile(root / "missing");
  require(mkfifo((root / "fifo").c_str(), 0600) == 0, "mkfifo");
  rejectFile(root / "fifo");
  {
    std::ofstream file(root / "replacement");
    file << "schema=1\n# replaced\n";
  }
  std::filesystem::rename(root / "replacement", path);
  require(readRuntimeConfigFile(path.string()).ends_with("# replaced\n"),
          "rename reload");
  {
    std::ofstream file(path);
    file << std::string(65537, 'x');
  }
  rejectFile(path);
}
}  // namespace

int main() {
  try {
    require(parseRuntimeConfigText("schema=1\n").empty(), "zero targets");
    const std::string entry =
        "target=blue out0@02:00:00:00:00:02 10.0.0.2:9001";
    auto result =
        parseRuntimeConfigText(" #comment\r\n schema=1\r\n\t" + entry + "\r\n");
    require(result.size() == 1 && result[0].id == "blue" &&
                result[0].egress == "out0",
            "tokens");
    for (const auto& text :
         {"", "schema=2\n", "schema=1\nschema=1\n", "schema=1 extra\n",
          "schema=1\nunknown=1", "schema=1\rtarget=x",
          "schema=1\ntarget=a out0@00:00:00:00:00:00 10.0.0.2:1",
          "schema=1\ntarget=a out0@01:00:00:00:00:02 10.0.0.2:1",
          "schema=1\ntarget=a out0@02:00:00:00:00:02 224.0.0.1:1",
          "schema=1\ntarget=a out0@02:00:00:00:00:02 10.0.0.2:0",
          "schema=1\ntarget=bad.id out0@02:00:00:00:00:02 10.0.0.2:1"})
      reject(text);
    reject("schema=1\n" + entry + "\n" + entry);
    reject("schema=1\n" + entry + " extra");
    reject(std::string("schema=1\n\0", 10));
    reject("schema=1\n" + std::string(1025, '#'));
    reject("schema=1\n" + std::string(256, '\n'));
    reject(std::string(65537, ' '));
    std::string maximum = "schema=1\n";
    for (unsigned index = 0; index < 64; ++index)
      maximum += "target=t" + std::to_string(index) +
                 " out0@02:00:00:00:00:02 10.0.0.2:9001\n";
    require(parseRuntimeConfigText(maximum).size() == 64,
            "64 parsed before interface duplicate validation");
    reject(maximum + "target=overflow out0@02:00:00:00:00:02 10.0.0.2:9001\n");
    auto first = l4lb::control::RuntimeTarget{"id", "before", {}, 1, 2};
    auto second = first;
    second.egress = "after";
    require(l4lb::control::sameRuntimeTarget(first, second),
            "interface rename canonical identity");
    second.backend.ifindex = 2;
    require(!l4lb::control::sameRuntimeTarget(first, second),
            "ifindex identity");
    const char* temp = std::getenv("TMPDIR");
    std::string pattern =
        (temp ? std::string(temp) : std::filesystem::current_path().string()) +
        "/runtime-config-XXXXXX";
    if (!mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
    try {
      fileCases(pattern);
    } catch (...) {
      std::filesystem::remove_all(pattern);
      throw;
    }
    std::filesystem::remove_all(pattern);
    std::cout << "runtime config " << checks << " checks PASS\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
