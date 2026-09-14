if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR "L4LB_BUILD_XDP requires a native Linux build (WSL2 supported).")
endif()

find_program(L4LB_BPF_CLANG NAMES clang clang-18 clang-19 clang-20)
if(NOT L4LB_BPF_CLANG OR NOT EXISTS "${L4LB_BPF_CLANG}")
  message(FATAL_ERROR "XDP: Clang not found. Install a Clang with BPF support or set L4LB_BPF_CLANG to its absolute path; use L4LB_BUILD_XDP=OFF for user-space only.")
endif()
set(L4LB_BPF_INCLUDE_DIRS "" CACHE STRING "Additional system include directories for BPF (semicolon-separated)")
set(_xdp_includes)
foreach(_dir IN LISTS L4LB_BPF_INCLUDE_DIRS)
  if(NOT IS_DIRECTORY "${_dir}")
    message(FATAL_ERROR "XDP: include directory does not exist: ${_dir}. Correct L4LB_BPF_INCLUDE_DIRS.")
  endif()
  list(APPEND _xdp_includes -isystem "${_dir}")
endforeach()
if(CMAKE_LIBRARY_ARCHITECTURE AND IS_DIRECTORY "/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}")
  list(APPEND _xdp_includes -isystem "/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}")
endif()

set(_xdp_flags -target bpf -O2 -g -Wall -Wextra -Werror ${_xdp_includes})
set(_xdp_source "${PROJECT_SOURCE_DIR}/src/xdp/xdp_pass.bpf.c")
set(_xdp_probe "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/l4lb-xdp-probe.o")
file(REMOVE "${_xdp_probe}")
execute_process(COMMAND "${L4LB_BPF_CLANG}" ${_xdp_flags} -c "${_xdp_source}" -o "${_xdp_probe}"
  RESULT_VARIABLE _xdp_result OUTPUT_VARIABLE _xdp_stdout ERROR_VARIABLE _xdp_stderr TIMEOUT 30)
if(NOT "${_xdp_result}" STREQUAL "0" OR NOT EXISTS "${_xdp_probe}")
  message(FATAL_ERROR "XDP: BPF compile probe failed with ${L4LB_BPF_CLANG} (${_xdp_result}). Check Clang BPF backend and Linux UAPI headers (linux/bpf.h, asm/types.h); set L4LB_BPF_INCLUDE_DIRS for additional header paths.\n${_xdp_stdout}${_xdp_stderr}")
endif()
# Reject compilers/wrappers that return success but produce a host object.
file(READ "${_xdp_probe}" _xdp_ident OFFSET 0 LIMIT 20 HEX)
if(NOT _xdp_ident MATCHES "^7f454c46020101[0-9a-f]+0100f700$" AND
   NOT _xdp_ident MATCHES "^7f454c46020201[0-9a-f]+000100f7$")
  message(FATAL_ERROR "XDP: compiler probe did not produce an ELF64 EM_BPF relocatable object. Check L4LB_BPF_CLANG.")
endif()
file(REMOVE "${_xdp_probe}")

set(_xdp_output "${CMAKE_CURRENT_BINARY_DIR}/xdp/xdp_pass.bpf.o")
set(_xdp_depfile "${CMAKE_CURRENT_BINARY_DIR}/xdp/xdp_pass.bpf.d")
add_custom_command(OUTPUT "${_xdp_output}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/xdp"
  COMMAND "${CMAKE_COMMAND}" -E rm -f "${_xdp_output}" "${_xdp_output}.tmp"
  COMMAND "${L4LB_BPF_CLANG}" ${_xdp_flags} -MD -MF "${_xdp_depfile}" -MQ "${_xdp_output}"
    -c "${_xdp_source}" -o "${_xdp_output}.tmp"
  COMMAND "${CMAKE_COMMAND}" -E rename "${_xdp_output}.tmp" "${_xdp_output}"
  DEPENDS "${_xdp_source}" "${L4LB_BPF_CLANG}" "${CMAKE_CURRENT_LIST_FILE}"
  DEPFILE "${_xdp_depfile}"
  BYPRODUCTS "${_xdp_depfile}"
  COMMENT "Building optional XDP_PASS BPF object" VERBATIM)
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${_xdp_output}.tmp")
add_custom_target(l4lb_xdp ALL DEPENDS "${_xdp_output}")
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME xdp_object COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/xdp_object_test.py" "${_xdp_output}")
  set_tests_properties(xdp_object PROPERTIES LABELS "xdp_build" TIMEOUT 15)
endif()
message(STATUS "XDP: enabled (${L4LB_BPF_CLANG}); target l4lb_xdp -> ${_xdp_output}")
