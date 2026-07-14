cmake_minimum_required(VERSION 3.20)
project(perf-dwarf-user C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# DWARF user callchains (PERF_SAMPLE_REGS_USER + PERF_SAMPLE_STACK_USER) exist
# precisely for binaries WITHOUT frame pointers, so this workload is built
# -O2 -fomit-frame-pointer -- the case where frame-pointer unwinding fails but
# the kernel's raw regs + stack dump still lets a host `perf report --call-graph
# dwarf` unwind. `noinline` keeps a real (if FP-less) call stack to dump. Plain C
# (no arch asm), so it builds on every arch; main() skips as a pass off aarch64.
add_executable(perf-dwarf-user src/perf_dwarf_user.c)
target_compile_options(perf-dwarf-user PRIVATE
    -Wall
    -Wextra
    -Werror
    -O2
    -fomit-frame-pointer
    -fno-optimize-sibling-calls
)
install(TARGETS perf-dwarf-user RUNTIME DESTINATION usr/bin/starry-test-suit)
