cmake_minimum_required(VERSION 3.20)
project(ftrace-function C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# The ftrace *function tracer* through tracefs (opt-in STARRY_FUNCTION_TRACER=1
# build → -Zpatchable-function-entry): filter a function, select current_tracer,
# trigger it, read `trace`:
#   echo <fn> > set_ftrace_filter ; echo function > current_tracer
#   <trigger>  ; cat trace  ->  `<fn>(<-caller)` records ; echo nop > current_tracer
# The kernel self-patches the function's 2-NOP entry into `mov x9,x30; bl
# ftrace_caller`. Only meaningful in a function_tracer build (guarded by
# availability of /sys/kernel/debug/tracing/current_tracer); prints OK otherwise.
add_executable(ftrace-function src/ftrace_function.c)
target_compile_options(ftrace-function PRIVATE -Wall -Wextra -Werror)
install(TARGETS ftrace-function RUNTIME DESTINATION usr/bin/starry-test-suit)
