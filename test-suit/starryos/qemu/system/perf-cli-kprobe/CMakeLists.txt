cmake_minimum_required(VERSION 3.20)
project(perf-cli-kprobe C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# Dynamic kprobe events via the tracefs kprobe_events interface -- the exact flow
# the classic `perf` CLI uses (`perf probe -a func` then `perf record -e
# probe:func`), driven here by raw file I/O + perf_event_open so it validates the
# kernel path without needing a libelf/libtraceevent perf binary:
#   1. write `p:probe/NAME <symbol>` to /sys/kernel/debug/tracing/kprobe_events
#   2. read events/probe/NAME/id
#   3. perf_event_open(PERF_TYPE_TRACEPOINT, config=id) + mmap + ENABLE
#   4. trigger the probed function; assert PERF_RECORD_SAMPLEs appear
# aarch64 only (reuses the ARM PMUv3 sampling ring); main() skips off aarch64.
add_executable(perf-cli-kprobe src/perf_cli_kprobe.c)
target_compile_options(perf-cli-kprobe PRIVATE -Wall -Wextra -Werror)
install(TARGETS perf-cli-kprobe RUNTIME DESTINATION usr/bin/starry-test-suit)
