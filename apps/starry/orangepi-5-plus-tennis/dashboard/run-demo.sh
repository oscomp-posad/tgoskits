#!/bin/sh
# Launch the STARRY//SIGNAL dashboard + live tennis loop on StarryOS (see
# DEMO.md). Telemetry goes over a pipe — do NOT switch to `--telemetry`
# file-tailing: concurrently tailing a tmpfs file the app writes can wedge the
# writer in-kernel (StarryOS fs-concurrency bug, reboot-only recovery).
# Override APP_DIR / HUD_DIR to match your deploy locations.
APP_DIR="${APP_DIR:-/tennis_app}"
HUD_DIR="${HUD_DIR:-/opt/hud}"
export LD_LIBRARY_PATH="$APP_DIR/lib:$HUD_DIR/lib"
export QT_QPA_PLATFORM=linuxfb
export QT_QPA_PLATFORM_PLUGIN_PATH="$HUD_DIR/plugins/platforms"
export QT_QPA_FONTDIR="$HUD_DIR/fonts"
export XDG_RUNTIME_DIR=/tmp
cd "$APP_DIR" || exit 1
exec ./tennis_app --mode live --virtual-actuators --duration-sec 3600 \
    --publish-camera /tmp/cam.rgba --publish-fps 10 2> /tmp/tennis.err \
    | "$HUD_DIR/dashboard" --camera /tmp/cam.rgba --fps 15
