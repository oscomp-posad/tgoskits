#!/usr/bin/env bash
# From-source `perf` build for CI. Runs the SAME reproducible recipe as
# build-perf.sh, but INSIDE the tgoskits base container (which already provides
# the aarch64-linux-musl cross toolchain) rather than wrapping `docker` -- CI jobs
# run in that container and have no docker daemon. Produces `./perf` next to this
# script (statically-linked aarch64), the path perf-tool-smoke's CMakeLists and the
# base-rootfs install (starry/rootfs.rs) consume. Idempotent: skips the build when
# `./perf` already exists.
#
# See build-perf.sh for the rationale behind linux-6.1, the musl cross toolchain,
# and the NO_LIBELF choice. libtraceevent is kept ENABLED (no NO_LIBTRACEEVENT):
# linux-6.1 still ships tools/lib/traceevent in-tree, so the cross build links it
# statically with no external dependency, giving `perf record`/`report`/`script`
# the tracing-data decode the perf-cli-e2e case exercises. (`perf probe` needs
# libelf and stays out -- perf-cli-e2e creates its kprobe via kprobe_events.)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PERF_VER="${PERF_VER:-6.1}"
WORK="${PERF_BUILD_DIR:-${RUNNER_TEMP:-/tmp}/perf-port-build}"

if [ -x "$HERE/perf" ]; then
  echo "perf already present: $HERE/perf"
  exit 0
fi

# flex/bison are perf build deps not baked into the base image; wget/xz fetch the
# kernel source. Best-effort apt update (the mirror can be flaky in CI).
apt-get update -qq >/dev/null 2>&1 || true
apt-get install -y -qq flex bison wget xz-utils >/dev/null

mkdir -p "$WORK"
cd "$WORK"
V="$PERF_VER"
[ -f "linux-$V.tar.xz" ] || wget -q "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$V.tar.xz"
[ -d "linux-$V" ] || tar xf "linux-$V.tar.xz"
cd "linux-$V/tools/perf"

make -j"$(nproc)" ARCH=arm64 CROSS_COMPILE=aarch64-linux-musl- \
  LDFLAGS="-static" EXTRA_CFLAGS="-Wno-error" \
  NO_LIBELF=1 NO_LIBDW=1 NO_DWARF=1 NO_LIBUNWIND=1 NO_LIBCAP=1 \
  NO_LIBBPF=1 NO_BPF_SKEL=1 NO_SLANG=1 NO_GTK2=1 NO_LIBPERL=1 \
  NO_LIBPYTHON=1 NO_LIBNUMA=1 NO_LIBCRYPTO=1 NO_LIBZSTD=1 \
  NO_LZMA=1 NO_ZLIB=1 NO_JVMTI=1 NO_LIBBABELTRACE=1 NO_AUXTRACE=1 \
  NO_LIBDEBUGINFOD=1 NO_LIBLLVM=1

aarch64-linux-musl-strip -o "$HERE/perf" perf
echo "built perf from linux-$V source: $HERE/perf ($(stat -c%s "$HERE/perf") bytes)"
