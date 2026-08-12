cmake_minimum_required(VERSION 3.20)
project(perf-sw-systemwide C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# System-wide (`perf stat -a`) software counters: a pid<0 SW event used to return
# ENOSYS; now cpu-clock / context-switches / page-faults open and aggregate
# machine-wide. Plain C; main() skips as a pass off aarch64.
add_executable(perf-sw-systemwide src/perf_sw_systemwide.c)
target_compile_options(perf-sw-systemwide PRIVATE -Wall -Wextra -Werror)
install(TARGETS perf-sw-systemwide RUNTIME DESTINATION usr/bin/starry-test-suit)
