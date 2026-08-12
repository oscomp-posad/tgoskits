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

# The stock Alpine rootfs image ships nearly full (~135 MiB free); the Qt
# runtime closure is ~276 MiB installed, so the guest apk install would run out
# of space (manifesting as partial installs / missing qt6-qtbase-x11 → no
# linuxfb plugin). Grow the ext4 image to a comfortable absolute size so the
# runtime fits. Idempotent: only grows if below target. Requires resize2fs
# (e2fsprogs — already required for debugfs) and BSD/GNU `dd` (portable seek).
grow_rootfs() {
    local img="$rootfs" target_mib=3072 cur_mib
    [ -f "$img" ] || { echo "warning: rootfs is not a file ($img); skip grow" >&2; return 0; }
    command -v resize2fs >/dev/null 2>&1 || { echo "warning: resize2fs missing; skip rootfs grow" >&2; return 0; }
    cur_mib=$(( $(wc -c <"$img") / 1048576 ))
    if [ "$cur_mib" -lt "$target_mib" ]; then
        echo "QT_PREBUILD growing rootfs ${cur_mib}MiB -> ${target_mib}MiB for Qt runtime..."
        dd if=/dev/zero bs=1048576 count=0 seek="$target_mib" of="$img" 2>/dev/null
        e2fsck -fy "$img" >/dev/null 2>&1 || true
        resize2fs "$img" >/dev/null 2>&1 || echo "warning: resize2fs failed (Qt install may run out of space)" >&2
    fi
}

alpine_branch() {
    local branch
    branch="$(sed -n 's#.*/\(v[0-9][0-9.]*\)/main#\1#p' "$overlay_dir/etc/apk/repositories" 2>/dev/null | head -1)"
    [[ -z "$branch" ]] && branch="v3.23"
    echo "$branch"
}

# Best-effort prefetch of the Qt *runtime* + fonts into the overlay, resolving
# the dependency closure from the Alpine APKINDEX. On-target the test script
# installs these with `apk add --no-network` first, then falls back to live
# mirrors. NOTE: the compiler/-dev packages are intentionally NOT prefetched —
# the clock binary is cross-built at prebuild time in a matching-branch Alpine
# container (see compile_clock_in_container), so the guest only needs runtime
# libs. This keeps the prefetch closure small (~tens of apks, not hundreds).
prefetch_qt_apks() {
    local apk_arch branch cache_dir guest_cache_dir
    case "$arch" in
        x86_64 | riscv64 | aarch64 | loongarch64) apk_arch="$arch" ;;
        *) echo "warning: unsupported apk arch for Qt prefetch: $arch" >&2; return 0 ;;
    esac

    branch="$(alpine_branch)"
    cache_dir="$workspace/target/qt-demo-apks/$branch/$apk_arch"
    guest_cache_dir="$overlay_dir/usr/local/qt-demo-apks"
    mkdir -p "$cache_dir" "$guest_cache_dir"

    if ! command -v python3 >/dev/null 2>&1; then
        echo "warning: python3 not found; skipping Qt APK prefetch (runtime apk will fetch from network)" >&2
        return 0
    fi

    QT_DEMO_ROOTS="${QT_DEMO_ROOTS:-qt6-qtbase qt6-qtbase-x11 font-dejavu fontconfig}" \
    python3 "$app_dir/prefetch_apks.py" "$apk_arch" "$branch" "$cache_dir" "$guest_cache_dir" || {
        echo "warning: Qt APK prefetch failed; runtime apk will fetch from network" >&2
        return 0
    }
}

# Cross-build the clock binary at prebuild time in an Alpine container whose
# branch matches the guest rootfs, so the resulting dynamically-linked binary is
# ABI-compatible with the guest's runtime Qt libs (verified: guest v3.23 and
# alpine:3.23 both ship qt6-qtbase-6.10.3-r0). Writes /usr/local/qt-demo/clock
# into the overlay. Best-effort: if docker is unavailable or the build fails,
# no binary is shipped and the on-target test script compiles from clock.cpp
# (which is also installed) as a fallback.
compile_clock_in_container() {
    local branch plat out_dir builder base_img prep
    branch="$(alpine_branch)"
    out_dir="$overlay_dir/usr/local/qt-demo"
    mkdir -p "$out_dir"

    if ! command -v docker >/dev/null 2>&1; then
        echo "warning: docker not found; shipping clock.cpp for on-target compile" >&2
        return 0
    fi
    case "$arch" in
        aarch64) plat="linux/arm64" ;;
        x86_64) plat="linux/amd64" ;;
        *) echo "warning: no container platform for arch $arch; on-target compile" >&2; return 0 ;;
    esac

    # Prefer a cached builder image with the Qt SDK preinstalled (instant
    # compile). Build it once if missing so subsequent runs are fast; the SDK
    # download is then a one-time, docker-layer-cached cost per machine. Fall
    # back to a plain alpine image + on-the-fly apk-add if the build is
    # unavailable.
    builder="qt-demo-builder:${branch#v}"
    base_img="alpine:${branch#v}"
    if ! docker image inspect "$builder" >/dev/null 2>&1; then
        echo "QT_PREBUILD building cached Qt builder image $builder (one-time)..."
        local ctx
        ctx="$(mktemp -d)"
        printf 'FROM %s\nRUN apk add --no-cache qt6-qtbase-dev g++ musl-dev pkgconf\n' "$base_img" >"$ctx/Dockerfile"
        if ! docker build --platform "$plat" -t "$builder" "$ctx" >/dev/null 2>&1; then
            echo "warning: builder image build failed; falling back to inline apk-add" >&2
            builder=""
        fi
        rm -rf "$ctx"
    fi

    if [[ -n "$builder" ]]; then
        prep=""            # SDK already in the image
        base_img="$builder"
    else
        prep='apk add --no-cache qt6-qtbase-dev g++ musl-dev pkgconf >/dev/null &&'
    fi

    echo "QT_PREBUILD compiling clock in $base_img ($plat)..."
    if docker run --rm --platform "$plat" \
        -v "$app_dir/clock.cpp:/src/clock.cpp:ro" \
        -v "$out_dir:/out" \
        "$base_img" sh -c "
            set -e
            ${prep} g++ -std=c++17 -fPIC -O2 /src/clock.cpp \
                \$(pkg-config --cflags --libs Qt6Widgets Qt6Gui Qt6Core) \
                -o /out/clock
            chmod 0755 /out/clock
        "; then
        echo "QT_PREBUILD compiled binary -> /usr/local/qt-demo/clock"
    else
        echo "warning: container compile failed; shipping clock.cpp for on-target compile" >&2
        rm -f "$out_dir/clock"
    fi
}

populate_overlay() {
    grow_rootfs
    mkdir -p "$overlay_dir/usr/bin" "$overlay_dir/usr/local/qt-demo"
    # Ship the source too, so the guest can compile as a fallback if the
    # container prebuild did not produce a binary.
    install -Dm0644 "$app_dir/clock.cpp" "$overlay_dir/usr/local/qt-demo/clock.cpp"
    install -Dm0755 "$app_dir/qt-demo-test.sh" "$overlay_dir/usr/bin/qt-demo-test.sh"
    # Stage-1 board framebuffer smoke test (no Qt); harmless in QEMU overlays too.
    install -Dm0755 "$app_dir/fb-smoke.sh" "$overlay_dir/usr/bin/fb-smoke.sh"

    copy_base_text_file_to_overlay /etc/apk/repositories
    copy_base_text_file_to_overlay /etc/resolv.conf
    prefetch_qt_apks
    compile_clock_in_container
}

require_env STARRY_ARCH "$arch"
require_env STARRY_ROOTFS "$rootfs"
require_env STARRY_OVERLAY_DIR "$overlay_dir"

ensure_host_tools
populate_overlay
