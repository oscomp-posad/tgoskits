cmake_minimum_required(VERSION 3.20)
project(perf-cli-e2e NONE)

# End-to-end: the REAL `perf` CLI recording + decoding a kernel-created kprobe on
# StarryOS (`perf record -e probe: ...` -> `perf report`/`perf script`). Needs a
# `perf` built WITH libtraceevent (see build-perf-ci.sh, which drops
# NO_LIBTRACEEVENT); the probe is created via the kernel's own kprobe_events, so
# libelf/`perf probe` is NOT required. The prebuilt lives at
# ../perf-tool-smoke/perf (gitignored). Silently skipped when absent so the
# qemu/system group still builds.
set(PERF_BIN "${CMAKE_CURRENT_SOURCE_DIR}/../perf-tool-smoke/perf")
if(NOT EXISTS "${PERF_BIN}")
    message(STATUS "perf-cli-e2e: prebuilt perf absent; skipping")
    return()
endif()

install(PROGRAMS "${PERF_BIN}" DESTINATION usr/bin)
install(PROGRAMS src/perf-cli-e2e.sh
    DESTINATION usr/bin/starry-test-suit
    RENAME perf-cli-e2e
)
