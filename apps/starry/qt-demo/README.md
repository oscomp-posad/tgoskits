# qt-demo

A stock Qt6 QtWidgets analog clock rendered to `/dev/fb0` via Qt's `linuxfb`
platform plugin. Stage 0 of the "Qt on StarryOS" effort: proves the Qt runtime
(dynamic linking, event loop, futex/threads, fbdev) on StarryOS under QEMU,
before any native RK3588 display driver exists.

## Run (QEMU, aarch64)

    cargo xtask starry rootfs --arch aarch64
    cargo xtask starry app qemu -t qt-demo --arch aarch64

The app installs Qt6 + g++ via `apk` at runtime, compiles `clock.cpp`
natively on-target, and runs it. Pass signal on serial: `QT_DEMO_PASSED`.

linuxfb is software-rendered (QtWidgets works; Qt Quick/QML would need GL and
is out of scope). Input is disabled (`QT_QPA_FB_DISABLE_INPUT=1`).
