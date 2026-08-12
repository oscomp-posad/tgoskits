#!/bin/sh
set -eu

# Stage-1 on-board framebuffer smoke test for the rockchip-vop2 driver.
# No Qt: just prove the VOP2-backed /dev/fb0 reaches the HDMI panel by filling
# the whole framebuffer with a solid colour. Dependency-light (busybox tr/head).

fail() { echo "FB_SMOKE_FAILED: $*"; echo "FB_SMOKE_FAILED"; exit 1; }

[ -e /dev/fb0 ] || fail "/dev/fb0 not present (rockchip-vop2 did not register a display)"
echo "FB_SMOKE /dev/fb0 present"

# 1920x1080 XRGB8888 = 8294400 bytes. Fill with solid white (0xFF) — an
# unmistakable full-screen flash on the monitor that proves CPU writes land in
# the scanout buffer VOP2 is fetching.
BYTES=8294400
if command -v tr >/dev/null 2>&1; then
    tr '\0' '\377' < /dev/zero | head -c "$BYTES" > /dev/fb0 2>/dev/null \
        || fail "framebuffer write failed"
else
    dd if=/dev/zero bs=1 count="$BYTES" 2>/dev/null | tr '\0' '\377' > /dev/fb0 2>/dev/null \
        || fail "framebuffer write failed"
fi
echo "FB_SMOKE wrote ${BYTES} bytes (white)"

echo "FB_SMOKE_PASSED"
