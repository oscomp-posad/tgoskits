#!/usr/bin/env bash
set -euo pipefail

app_dir="${STARRY_APP_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
workspace="${STARRY_WORKSPACE:-$(cd "$app_dir/../../.." && pwd)}"
arch="${STARRY_ARCH:-}"
rootfs="${STARRY_ROOTFS:-}"
overlay_dir="${STARRY_OVERLAY_DIR:-}"

require_env() {
    local name="$1" value="$2"
    if [[ -z "$value" ]]; then
        echo "error: $name is required" >&2
        exit 1
    fi
}

ensure_host_tools() {
    command -v debugfs >/dev/null 2>&1 || {
        echo "error: missing required host package: e2fsprogs (debugfs)" >&2
        exit 1
    }
}

copy_base_text_file_to_overlay() {
    local guest_path="$1"
    local target="$overlay_dir$guest_path"
    mkdir -p "$(dirname "$target")"
    if ! debugfs -R "cat $guest_path" "$rootfs" >"$target" 2>/dev/null; then
        rm -f "$target"
        return
    fi
    chmod 0644 "$target"
}

# Best-effort prefetch of the Qt runtime + toolchain + fonts into the overlay,
# resolving the full dependency closure from the Alpine APKINDEX. On-target the
# test script installs these with `apk add --no-network` first, then falls back
# to live mirrors. Reuses the wayland resolver approach.
prefetch_qt_apks() {
    local apk_arch branch cache_dir guest_cache_dir
    case "$arch" in
        x86_64 | riscv64 | aarch64 | loongarch64) apk_arch="$arch" ;;
        *) echo "warning: unsupported apk arch for Qt prefetch: $arch" >&2; return 0 ;;
    esac

    branch="$(sed -n 's#.*/\(v[0-9][0-9.]*\)/main#\1#p' "$overlay_dir/etc/apk/repositories" 2>/dev/null | head -1)"
    [[ -z "$branch" ]] && branch="v3.22"

    cache_dir="$workspace/target/qt-demo-apks/$branch/$apk_arch"
    guest_cache_dir="$overlay_dir/usr/local/qt-demo-apks"
    mkdir -p "$cache_dir" "$guest_cache_dir"

    if ! command -v python3 >/dev/null 2>&1; then
        echo "warning: python3 not found; skipping Qt APK prefetch (runtime apk will fetch from network)" >&2
        return 0
    fi

    QT_DEMO_ROOTS="${QT_DEMO_ROOTS:-qt6-qtbase qt6-qtbase-x11 font-dejavu fontconfig g++ musl-dev qt6-qtbase-dev}" \
    python3 "$app_dir/prefetch_apks.py" "$apk_arch" "$branch" "$cache_dir" "$guest_cache_dir" || {
        echo "warning: Qt APK prefetch failed; runtime apk will fetch from network" >&2
        return 0
    }
}

populate_overlay() {
    mkdir -p "$overlay_dir/usr/bin" "$overlay_dir/usr/local/qt-demo"
    install -Dm0644 "$app_dir/clock.cpp" "$overlay_dir/usr/local/qt-demo/clock.cpp"
    install -Dm0755 "$app_dir/qt-demo-test.sh" "$overlay_dir/usr/bin/qt-demo-test.sh"

    copy_base_text_file_to_overlay /etc/apk/repositories
    copy_base_text_file_to_overlay /etc/resolv.conf
    prefetch_qt_apks
}

require_env STARRY_ARCH "$arch"
require_env STARRY_ROOTFS "$rootfs"
require_env STARRY_OVERLAY_DIR "$overlay_dir"

ensure_host_tools
populate_overlay
