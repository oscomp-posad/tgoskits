cmake_minimum_required(VERSION 3.20)
project(perf-tracepoint-sample C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# tracepoint PERF_RECORD_SAMPLE emission: opening a PERF_TYPE_TRACEPOINT event
# with a sampling ring makes each tracepoint hit write a sample
# (`perf record -e syscalls:sys_enter_openat`, via the raw syscall). Resolves the
# tracepoint id from debugfs, probes it, and asserts samples appear attributed to
# the triggering task. aarch64 only (reuses the ARM PMUv3 sampling ring); main()
# skips off aarch64.
add_executable(perf-tracepoint-sample src/perf_tracepoint_sample.c)
target_compile_options(perf-tracepoint-sample PRIVATE -Wall -Wextra -Werror)
install(TARGETS perf-tracepoint-sample RUNTIME DESTINATION usr/bin/starry-test-suit)
