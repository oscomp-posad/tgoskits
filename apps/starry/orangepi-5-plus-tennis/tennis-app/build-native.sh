#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
build="$root/build-native"
mkdir -p "$build"

includes=(
    -I"$root/cpp"
    -I"$root/cpp/rknpu2"
    -I"$root/utils"
    -I"$root/3rdparty/rknpu2/include"
    -I"$root/3rdparty/jpeg_turbo/include"
    -I"$root/3rdparty/librga/include"
    -I"$root/3rdparty/mpp/include"
    -I"$root/3rdparty/stb_image"
)

gcc -O3 -DNDEBUG -std=gnu11 -DLIBRGA_IM2D_HANDLE "${includes[@]}" \
    -c "$root/utils/image_utils.c" -o "$build/image_utils.o"
gcc -O3 -DNDEBUG -std=gnu11 "${includes[@]}" \
    -c "$root/utils/file_utils.c" -o "$build/file_utils.o"

sources=(
    cpp/main.cc
    cpp/controller.cc
    cpp/odometry.cc
    cpp/odometry_worker.cc
    cpp/state_machine.cc
    cpp/bench/metrics.cc
    cpp/actuator/actuator_factory.cc
    cpp/actuator/trace_motor_backend.cc
    cpp/actuator/trace_arm_backend.cc
    cpp/live.cc
    cpp/perception/camera.cc
    cpp/perception/tennis_detector.cc
    cpp/perception/jpu_decoder.cc
    cpp/perception/bucket_detector.cc
    cpp/actuator/serial_device.cc
    cpp/actuator/pwm_motor_backend.cc
    cpp/actuator/uart_motor_backend.cc
    cpp/actuator/uart_arm_backend.cc
    cpp/uvc_capture.cc
    cpp/rknpu2/yolov8.cc
    cpp/postprocess.cc
)

objects=("$build/image_utils.o" "$build/file_utils.o")
for source in "${sources[@]}"; do
    object="$build/${source//\//_}.o"
    g++ -O3 -DNDEBUG -std=gnu++17 "${includes[@]}" \
        -c "$root/$source" -o "$object"
    objects+=("$object")
done

g++ -O3 -static-libstdc++ -static-libgcc -o "$root/tennis_app" \
    "${objects[@]}" \
    "$root/3rdparty/rknpu2/Linux/aarch64/librknnrt.so" \
    "$root/3rdparty/jpeg_turbo/Linux/aarch64/libturbojpeg.a" \
    "$root/3rdparty/librga/Linux/aarch64/librga.a" \
    "$root/3rdparty/mpp/lib/librockchip_mpp.so" \
    -ldl -pthread -Wl,-rpath,'$ORIGIN/lib'

printf 'built: %s\n' "$root/tennis_app"
