cmake_minimum_required(VERSION 3.20)
project(ftrace-kprobe C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# The classic ftrace kprobe flow through tracefs (no perf, no BPF):
#   echo 'p:probe/hsc <sym>' > kprobe_events   (create a dynamic kprobe event)
#   echo 1 > events/probe/hsc/enable           (arm the kprobe)
#   <trigger syscalls>                         (each hits handle_syscall)
#   cat trace                                  (formatted records: `hsc(0x<ip>)`)
#   echo 0 > enable ; echo '-:probe/hsc' > kprobe_events
# This exercises the enable->trace-ring path, which is independent of
# perf_event_open. aarch64-only (kprobe backend); other arches print OK.
add_executable(ftrace-kprobe src/ftrace_kprobe.c)
target_compile_options(ftrace-kprobe PRIVATE -Wall -Wextra -Werror)
install(TARGETS ftrace-kprobe RUNTIME DESTINATION usr/bin/starry-test-suit)
