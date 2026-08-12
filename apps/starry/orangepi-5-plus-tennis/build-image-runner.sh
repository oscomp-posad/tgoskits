#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Cross-compile the tennis_app native runner for the OrangePi-5-Plus
# (aarch64 Linux glibc). Run on a Linux host with an aarch64-linux-gnu toolchain.
# The result is installed under install/rk3588_linux_aarch64/tennis_app and must
# be copied onto the board rootfs (see README.md "On-board install").
#
# The build is fully self-contained: all RKNN/UVC/RGA/utils code is vendored under
# tennis-app/ (3rdparty/, utils/, cpp/). No sibling app is required.
set -euo pipefail

case_dir="$(cd "$(dirname "$0")" && pwd)"
src_dir="${case_dir}/tennis-app"
build_dir="${src_dir}/build-rk3588-aarch64"
install_dir="${src_dir}/install/rk3588_linux_aarch64/tennis_app"
# Auto-detect the aarch64 Linux (glibc) cross prefix if CROSS_COMPILE is unset:
# prefer the standard aarch64-linux-gnu- (Linux host / tgoskits container), then
# fall back to aarch64-unknown-linux-gnu- (the Homebrew messense toolchain on macOS).
if [ -n "${CROSS_COMPILE:-}" ]; then
  cross_prefix="${CROSS_COMPILE}"
elif command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
  cross_prefix="aarch64-linux-gnu-"
elif command -v aarch64-unknown-linux-gnu-gcc >/dev/null 2>&1; then
  cross_prefix="aarch64-unknown-linux-gnu-"
else
  echo "error: no aarch64 Linux (glibc) cross toolchain found." >&2
  echo "  Linux/Debian: apt-get install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu" >&2
  echo "  macOS:        brew install messense/macos-cross-toolchains/aarch64-unknown-linux-gnu" >&2
  echo "  or set CROSS_COMPILE=<prefix-> explicitly." >&2
  exit 1
fi
cc="${CC:-${cross_prefix}gcc}"
cxx="${CXX:-${cross_prefix}g++}"
jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

command -v "${cc}" >/dev/null
command -v "${cxx}" >/dev/null

rm -rf "${build_dir}" "${install_dir}"
mkdir -p "${build_dir}" "${install_dir}"

cmake -S "${src_dir}" -B "${build_dir}" \
  -DCMAKE_C_COMPILER="${cc}" \
  -DCMAKE_CXX_COMPILER="${cxx}" \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${install_dir}" \
  -DTARGET_SOC=rk3588

cmake --build "${build_dir}" -j"${jobs}"
cmake --install "${build_dir}"

echo "installed: ${install_dir}"
