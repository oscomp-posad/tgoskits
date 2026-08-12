cmake_minimum_required(VERSION 3.20)
project(perf-top-smoke NONE)

# Reuse the prebuilt `perf` from the sibling perf-tool-smoke case (built by
# build-perf.sh / the CI perf build job; also shipped in the base rootfs). Install
# it if present so this case is self-contained; the script skips-as-pass when the
# binary is absent (e.g. CI without a local build).
set(PERF_BIN "${CMAKE_CURRENT_SOURCE_DIR}/../perf-tool-smoke/perf")
if(EXISTS "${PERF_BIN}")
    install(PROGRAMS "${PERF_BIN}" DESTINATION usr/bin)
endif()

install(PROGRAMS src/perf-top-smoke.sh
    DESTINATION usr/bin/starry-test-suit
    RENAME perf-top-smoke
)
