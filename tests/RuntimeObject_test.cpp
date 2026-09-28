#include <bpf/libbpf.h>

#include <iostream>
#include <memory>
#include <string>

#include "xdp/RuntimeMapStore.h"

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const bool reject = std::string(argv[1]) == "--reject";
  auto* opened = bpf_object__open_file(argv[2], nullptr);
  // ELF/BTF must open successfully: failure here is not our validator rejecting
  // a well-formed object with forbidden metadata.
  if (!opened || libbpf_get_error(opened)) {
    std::cerr << "object open failed before project validation\n";
    return 1;
  }
  std::unique_ptr<bpf_object, decltype(&bpf_object__close)> object(
      opened, bpf_object__close);
  try {
    l4lb::xdp::RuntimeMapStore::validateObject(object.get());
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    std::cerr << message << '\n';
    return reject && (message.starts_with("runtime ") ||
                      message.starts_with("缺少 runtime "))
               ? 0
               : 1;
  }
  if (reject) std::cerr << "malformed object was accepted\n";
  return reject ? 1 : 0;
}
