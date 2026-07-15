cmake_minimum_required(VERSION 3.20)
project(ftrace-tracepoint C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# ftrace static-tracepoint tracing via tracefs: `echo 1 >
# events/syscalls/sys_enter_openat/enable`, trigger the event, then read
# /sys/kernel/debug/tracing/trace and confirm the formatted records appear
# (the achievable ftrace surface on StarryOS; there is no function tracer /
# current_tracer, which would need -pg/mcount instrumentation the kernel lacks).
# This is also the regression test for the trace-to-pipe deadlock: an enabled
# tracepoint pushes into the ring from the preempt-disabled fire path, which used
# to take a sleeping mutex and wedge the CPU on the first hit.
# Portable across arches (tracefs is arch-independent).
add_executable(ftrace-tracepoint src/ftrace_tracepoint.c)
target_compile_options(ftrace-tracepoint PRIVATE -Wall -Wextra -Werror)
install(TARGETS ftrace-tracepoint RUNTIME DESTINATION usr/bin/starry-test-suit)
