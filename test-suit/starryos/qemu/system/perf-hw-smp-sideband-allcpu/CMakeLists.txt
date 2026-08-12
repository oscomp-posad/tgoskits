cmake_minimum_required(VERSION 3.20)
project(perf-hw-smp-sideband-allcpu C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# System-wide (`perf record -a`) side-band fan-out: one `-a` sampling event per cpu
# (pid=-1, cpu=i) must ALSO receive COMM/MMAP2/FORK/EXIT for whatever runs on core
# i, so a whole-machine capture is symbolizable. Opens+enables a `-a` event per
# cpu, forks a child that execs itself and exits, and asserts the records land in
# the per-cpu rings. Hardware PMU (ARM PMUv3); main() skips as a pass off aarch64.
add_executable(perf-hw-smp-sideband-allcpu src/perf_hw_smp_sideband_allcpu.c)
target_compile_options(perf-hw-smp-sideband-allcpu PRIVATE -Wall -Wextra -Werror)
install(TARGETS perf-hw-smp-sideband-allcpu RUNTIME DESTINATION usr/bin/starry-test-suit)
