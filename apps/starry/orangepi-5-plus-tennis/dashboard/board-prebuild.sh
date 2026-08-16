#!/usr/bin/env bash
# Build the STARRY//SIGNAL dashboard (dashboard.cpp) into a self-contained glibc
# Qt6 bundle for the OrangePi-5-Plus board running StarryOS. Renders fullscreen
# to the native VOP2 /dev/fb0 via linuxfb; also supports an offscreen PNG mode
# (--shot) for headless visual checks. Bundles Fira Code + Oswald so the HUD
# looks right regardless of the board's fontconfig.
#
# Output: <out>/dashboard-board.tar.gz  (dashboard + lib/ + plugins/ + fonts/)
# On the board (shared ext4), run under StarryOS:
#   cd <dir> && LD_LIBRARY_PATH=$PWD/lib QT_QPA_PLATFORM=linuxfb \
#     QT_QPA_PLATFORM_PLUGIN_PATH=$PWD/plugins/platforms XDG_RUNTIME_DIR=/tmp ./dashboard
#   # with the robot (non-interfering: SCHED_IDLE pinned to A55 so it never
#   # steals cycles from the tennis app), pipe the telemetry:
#   #   tennis_app --mode live … | (…env…) ./dashboard
#   # Do NOT `--telemetry`-tail a tmpfs file the app is writing on StarryOS —
#   # the concurrent tail can wedge the writer in-kernel (see DEMO.md).
#   # Turn-key: see run-demo.sh + DEMO.md in this directory.
set -euo pipefail
app_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out_dir="${1:-$app_dir/../../../../target/dashboard-board}"
plat="linux/arm64"
img="qt-hud-builder:22.04"

# builder image with Qt6 + Fira Code + Oswald baked in
if ! docker image inspect "$img" >/dev/null 2>&1; then
  echo "board-prebuild: building $img …"
  docker build --platform "$plat" -t "$img" - <<'EOF'
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ qt6-base-dev qt6-qpa-plugins libgl1 fonts-firacode curl ca-certificates fontconfig \
      fonts-noto-cjk python3-fonttools \
 && mkdir -p /usr/share/fonts/truetype/hud \
 && curl -sL -o /usr/share/fonts/truetype/hud/Oswald.ttf "https://github.com/google/fonts/raw/main/ofl/oswald/Oswald%5Bwght%5D.ttf" \
 && fc-cache -f && rm -rf /var/lib/apt/lists/*
EOF
fi

mkdir -p "$out_dir"; cp "$app_dir/dashboard.cpp" "$out_dir/dashboard.cpp"
rm -rf "$out_dir/out"; mkdir -p "$out_dir/out"
cat > "$out_dir/build.sh" <<'EOS'
set -e
QTINC=/usr/include/aarch64-linux-gnu/qt6
g++ -std=c++17 -fPIC -O2 /work/dashboard.cpp \
  -I$QTINC -I$QTINC/QtWidgets -I$QTINC/QtGui -I$QTINC/QtCore \
  -lQt6Widgets -lQt6Gui -lQt6Core -o /work/out/dashboard
mkdir -p /work/out/lib /work/out/plugins/platforms /work/out/fonts
for pl in libqlinuxfb.so libqoffscreen.so libqminimal.so; do
  f=$(find /usr/lib -name "$pl" | head -1); [ -n "$f" ] && cp -L "$f" /work/out/plugins/platforms/; done
cp /usr/share/fonts/truetype/firacode/FiraCode-Regular.ttf /usr/share/fonts/truetype/firacode/FiraCode-Bold.ttf /work/out/fonts/ 2>/dev/null || true
cp /usr/share/fonts/truetype/hud/Oswald.ttf /work/out/fonts/ 2>/dev/null || true
# Subset the Simplified-Chinese face of the Noto Sans CJK OTC down to just the
# glyphs this dashboard uses. Keeps the board bundle tiny (~0.3 MB vs ~40 MB for
# the full OTC) while guaranteeing correct SC glyph forms (测/见/应/负 …) — the JP
# default face would render some of these as traditional variants or tofu.
python3 - <<'PY'
s = open('/work/dashboard.cpp', encoding='utf-8').read()
open('/work/glyphs.txt', 'w', encoding='utf-8').write(''.join(sorted({c for c in s if ord(c) > 0x7f})))
PY
NOTO=/usr/share/fonts/opentype/noto
for wf in Regular Bold; do
  ttc="$NOTO/NotoSansCJK-$wf.ttc"
  idx=$(python3 -c "from fontTools.ttLib import TTCollection;c=TTCollection('$ttc');print(next(i for i,f in enumerate(c.fonts) if f['name'].getDebugName(1)=='Noto Sans CJK SC'))")
  python3 -m fontTools.subset "$ttc" --font-number="$idx" --text-file=/work/glyphs.txt --unicodes=U+0020-007E \
    --output-file="/work/out/fonts/NotoSansCJKsc-$wf.otf" --no-hinting --desubroutinize \
    && echo "subset Noto Sans CJK SC $wf (face $idx): $(du -h /work/out/fonts/NotoSansCJKsc-$wf.otf | cut -f1)"
done
gather() { ldd "$1" 2>/dev/null | awk '/=>/ {print $3}' | grep -E '^/' || true; }
{ gather /work/out/dashboard; for p in /work/out/plugins/platforms/*.so; do gather "$p"; done; } | sort -u | while read so; do
  case "$so" in */libc.so.6|*/ld-linux-*|*/libm.so.6|*/libpthread.so.0|*/libdl.so.2|*/librt.so.1|*/libresolv.so.2) : ;; *) cp -L "$so" /work/out/lib/ ;; esac
done
echo "dashboard + $(ls /work/out/lib | wc -l) libs + $(ls /work/out/fonts | wc -l) fonts"
EOS
docker run --rm --platform "$plat" -v "$out_dir:/work" "$img" bash /work/build.sh
tar czf "$out_dir/dashboard-board.tar.gz" -C "$out_dir/out" .
echo "board-prebuild: wrote $out_dir/dashboard-board.tar.gz"
