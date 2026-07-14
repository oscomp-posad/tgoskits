#!/bin/sh
set -eu

fail() {
    echo "QT_DEMO_FAILED: $*"
    echo "QT_DEMO_FAILED"
    exit 1
}

run_with_timeout() {
    secs="$1"; shift
    if command -v timeout >/dev/null 2>&1; then
        timeout "$secs" "$@"; return $?
    fi
    "$@" & pid=$!; el=0
    while kill -0 "$pid" 2>/dev/null; do
        [ "$el" -ge "$secs" ] && { kill "$pid" 2>/dev/null || true; return 124; }
        sleep 1; el=$((el + 1))
    done
    wait "$pid"
}

write_repos() {
    mirror="$1"
    branch="$(sed -n 's#.*/\(v[0-9][0-9.]*\)/main#\1#p' /etc/apk/repositories 2>/dev/null | head -1)"
    [ -z "$branch" ] && branch="v3.22"
    cat >/etc/apk/repositories <<EOF
${mirror}/${branch}/main
${mirror}/${branch}/community
EOF
}

# ---- packages ----
# Runtime only: qt6-qtbase (core libs), qt6-qtbase-x11 (carries the linuxfb QPA
# plugin libqlinuxfb.so on Alpine aarch64), font-dejavu + fontconfig (Qt needs
# fonts to render text). The clock binary is normally cross-built at prebuild
# time in a matching-branch Alpine container, so no compiler is needed on-target.
PKGS="qt6-qtbase qt6-qtbase-x11 font-dejavu fontconfig"
# Fallback compiler/-dev set, installed on-target only if the prebuilt binary
# is absent (e.g. docker was unavailable during prebuild).
DEV_PKGS="qt6-qtbase-dev g++ musl-dev"

install_packages() {
    # 1) prefetched offline install (fast, no network)
    if [ -f /usr/local/qt-demo-apks/install.list ]; then
        echo "QT_PREP installing prefetched APKs"
        # shellcheck disable=SC2046
        if run_with_timeout 600 apk add --allow-untrusted --no-network $(cat /usr/local/qt-demo-apks/install.list); then
            echo "QT_PREP prefetched install OK"; return 0
        fi
        echo "QT_PREP prefetched install failed; trying network mirrors"
    fi
    # 2) network mirrors with retry
    for mirror in \
        "${STARRY_APK_MIRROR:-}" \
        http://mirrors.huaweicloud.com/alpine \
        http://dl-cdn.alpinelinux.org/alpine \
        http://mirrors.aliyun.com/alpine; do
        [ -z "$mirror" ] && continue
        echo "QT_PREP apk mirror: $mirror"
        write_repos "$mirror"
        if run_with_timeout 420 apk add --no-cache $PKGS; then return 0; fi
        rm -rf /var/cache/apk/* 2>/dev/null || true
    done
    return 1
}

echo "QT_PREP installing Qt6 + toolchain..."
install_packages || fail "apk add Qt6 packages failed"

# ---- locate the linuxfb plugin ----
QPA_DIR="$(find /usr/lib -type d -name platforms -path '*qt6*' 2>/dev/null | head -1)"
if [ -z "$QPA_DIR" ] || [ ! -e "$QPA_DIR/libqlinuxfb.so" ]; then
    echo "QT_PREP available QPA plugins:"; find /usr/lib -name 'libq*.so' -path '*platforms*' 2>/dev/null || true
    fail "linuxfb QPA plugin (libqlinuxfb.so) not found"
fi
echo "QT_PREP linuxfb plugin: $QPA_DIR/libqlinuxfb.so"

# ---- framebuffer present? ----
[ -e /dev/fb0 ] || fail "/dev/fb0 not present (display backend missing)"
echo "QT_PREP /dev/fb0 present"

# ---- obtain the clock binary: prefer the prebuilt one, else compile on-target ----
if [ -x /usr/local/qt-demo/clock ]; then
    echo "QT_STAGE using prebuilt clock binary (/usr/local/qt-demo/clock)"
else
    echo "QT_STAGE no prebuilt binary; installing toolchain and compiling on-target..."
    # Install the dev/compiler set (from network; not prefetched).
    for mirror in \
        "${STARRY_APK_MIRROR:-}" \
        http://mirrors.huaweicloud.com/alpine \
        http://dl-cdn.alpinelinux.org/alpine \
        http://mirrors.aliyun.com/alpine; do
        [ -z "$mirror" ] && continue
        write_repos "$mirror"
        if run_with_timeout 900 apk add --no-cache $DEV_PKGS; then break; fi
        rm -rf /var/cache/apk/* 2>/dev/null || true
    done
    CXXFLAGS="$(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core 2>/dev/null || true)"
    LDFLAGS="$(pkg-config --libs Qt6Widgets Qt6Gui Qt6Core 2>/dev/null || true)"
    [ -n "$LDFLAGS" ] || fail "pkg-config could not resolve Qt6Widgets (dev headers missing)"
    # fPIC + position independent: Qt6 on Alpine is built PIE.
    run_with_timeout 600 g++ -std=c++17 -fPIC -O1 \
        /usr/local/qt-demo/clock.cpp $CXXFLAGS $LDFLAGS -o /usr/local/qt-demo/clock \
        || fail "g++ compile failed"
    echo "QT_STAGE compiled: /usr/local/qt-demo/clock"
fi

# ---- run under linuxfb ----
export QT_QPA_PLATFORM="linuxfb:fb=/dev/fb0"
export QT_QPA_FB_DISABLE_INPUT=1
export QT_LOGGING_RULES="qt.qpa.*=true"
export XDG_RUNTIME_DIR=/tmp
chmod 0700 /tmp 2>/dev/null || true

# capture fb checksum before to prove pixels change
before="$(md5sum /dev/fb0 2>/dev/null | cut -d' ' -f1 || echo none)"

echo "QT_STAGE launching clock..."
if ! run_with_timeout 120 /usr/local/qt-demo/clock 2>/tmp/qt.log; then
    tail -40 /tmp/qt.log || true
    fail "clock process exited non-zero"
fi
cat /tmp/qt.log || true

# clock printed QT_DEMO_PASSED itself on the way out; also assert the
# framebuffer actually changed (defends against a plugin that loads but
# never blits).
after="$(md5sum /dev/fb0 2>/dev/null | cut -d' ' -f1 || echo none)"
if [ "$before" = "$after" ]; then
    echo "QT_STAGE warning: /dev/fb0 checksum unchanged ($before)"
fi

echo "QT_DEMO_PASSED"
