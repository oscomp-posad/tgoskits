cmake_minimum_required(VERSION 3.20)
project(perf-kprobe-sample C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# kprobe PERF_RECORD_SAMPLE emission: opening a PERF_TYPE_KPROBE event with a
# sampling ring makes each probe hit write a sample (`perf record -e kprobe:func`,
# via the raw syscall). Probes the syscall dispatcher and asserts samples appear.
# aarch64 only (reuses the ARM PMUv3 sampling ring); main() skips off aarch64.
add_executable(perf-kprobe-sample src/perf_kprobe_sample.c)
target_compile_options(perf-kprobe-sample PRIVATE -Wall -Wextra -Werror)
install(TARGETS perf-kprobe-sample RUNTIME DESTINATION usr/bin/starry-test-suit)
