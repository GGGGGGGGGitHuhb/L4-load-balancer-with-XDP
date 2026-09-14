# Keep loader dependencies separate from the S1 BPF-only build.
if(NOT L4LB_BUILD_XDP)
  message(FATAL_ERROR "XDP loader: enable L4LB_BUILD_XDP=ON as well, or disable L4LB_BUILD_XDP_LOADER.")
endif()
find_path(L4LB_LIBBPF_INCLUDE_DIR bpf/libbpf.h)
find_library(L4LB_LIBBPF_LIBRARY NAMES bpf)
if(NOT L4LB_LIBBPF_INCLUDE_DIR OR NOT L4LB_LIBBPF_LIBRARY)
  message(FATAL_ERROR "XDP loader: libbpf development files missing. Install libbpf-dev (Ubuntu), or set L4LB_LIBBPF_INCLUDE_DIR and L4LB_LIBBPF_LIBRARY. BPF-only builds can keep L4LB_BUILD_XDP_LOADER=OFF.")
endif()
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_INCLUDES "${L4LB_LIBBPF_INCLUDE_DIR}")
set(CMAKE_REQUIRED_LIBRARIES "${L4LB_LIBBPF_LIBRARY}")
# Recheck if users replace an explicitly supplied dependency after a failure.
unset(L4LB_LIBBPF_USABLE CACHE)
check_cxx_source_compiles("
#include <bpf/libbpf.h>
#include <bpf/libbpf_version.h>
#include <bpf/bpf.h>
#if LIBBPF_MAJOR_VERSION < 1
#error libbpf >= 1.0 is required
#endif
int main() {
  bpf_xdp_attach_opts opts{}; opts.sz = sizeof(opts); opts.old_prog_fd = -1;
  bpf_xdp_detach(0, 0, &opts);
  return bpf_prog_get_fd_by_id(0);
}" L4LB_LIBBPF_USABLE)
cmake_pop_check_state()
if(NOT L4LB_LIBBPF_USABLE)
  message(FATAL_ERROR "XDP loader: need compatible libbpf >= 1.0 headers and library (including transitive dependencies for static linking). Check L4LB_LIBBPF_INCLUDE_DIR/L4LB_LIBBPF_LIBRARY or install libbpf-dev.")
endif()
add_executable(l4lb-xdp src/xdp/main.cpp src/xdp/loader.cpp)
target_include_directories(l4lb-xdp PRIVATE "${L4LB_LIBBPF_INCLUDE_DIR}")
target_link_libraries(l4lb-xdp PRIVATE "${L4LB_LIBBPF_LIBRARY}")
target_compile_options(l4lb-xdp PRIVATE -Wall -Wextra -Wpedantic)
add_dependencies(l4lb-xdp l4lb_xdp)
if(BUILD_TESTING)
  add_test(NAME xdp_loader_cli COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/xdp_loader_test.py"
    --clang "${L4LB_BPF_CLANG}" --loader "$<TARGET_FILE:l4lb-xdp>" --object "${CMAKE_CURRENT_BINARY_DIR}/xdp/xdp_pass.bpf.o")
  set_tests_properties(xdp_loader_cli PROPERTIES LABELS "xdp_loader" TIMEOUT 30)
endif()
