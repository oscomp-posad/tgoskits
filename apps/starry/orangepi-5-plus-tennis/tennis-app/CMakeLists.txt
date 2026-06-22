# SPDX-License-Identifier: Apache-2.0
#
# Build for the StarryOS tennis-ball pickup demo/benchmark (tennis_app).
#
# Two build modes:
#   * Board (default): cross-compiled aarch64-linux-gnu Linux binary that runs on
#     the OrangePi-5-Plus rootfs under StarryOS. It REUSES the vendored RKNN/UVC
#     code from the sibling orangepi-5-plus-uvc-rknn app (Apache-2.0, Rockchip)
#     instead of duplicating the multi-MB libraries/headers/model. See
#     TENNIS_RKNN_SHARED_DIR below. Built via ../build-image-runner.sh.
#   * Host dry-run (-DTENNIS_HOST_DRYRUN=ON): a self-contained native build with
#     no RKNN/UVC/RGA dependency. Only `--mode dry-run` works; it exists so the
#     mandatory TENNIS_BENCH_RESULT output is reproducible on any host (and in CI)
#     without a board or model.
cmake_minimum_required(VERSION 3.15)

project(tennis_app C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(TENNIS_HOST_DRYRUN "Build only the host dry-run (no RKNN/UVC/RGA)" OFF)

# Tennis-specific sources that are safe to compile on any host (no RKNN/UVC).
set(TENNIS_CORE_SRC
    cpp/main.cc
    cpp/controller.cc
    cpp/state_machine.cc
    cpp/bench/metrics.cc
    cpp/actuator/trace_motor_backend.cc
    cpp/actuator/trace_arm_backend.cc
)

if(TENNIS_HOST_DRYRUN)
    # --- Host dry-run build: state machine + backends + metrics + synthetic scene.
    add_executable(tennis_app ${TENNIS_CORE_SRC})
    target_compile_definitions(tennis_app PRIVATE TENNIS_HOST_DRYRUN)
    target_include_directories(tennis_app PRIVATE cpp)
    target_compile_options(tennis_app PRIVATE -Wall -Wextra)
else()
    # --- Board build: reuse the sibling uvc-rknn vendored code, do not duplicate.
    set(TENNIS_RKNN_SHARED_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../../orangepi-5-plus-uvc-rknn/rknn-yolov8-image"
        CACHE PATH "Path to the orangepi-5-plus-uvc-rknn native runner (reused RKNN/UVC/RGA code)")

    if(NOT EXISTS "${TENNIS_RKNN_SHARED_DIR}/3rdparty/CMakeLists.txt")
        message(FATAL_ERROR
            "TENNIS_RKNN_SHARED_DIR='${TENNIS_RKNN_SHARED_DIR}' does not contain the "
            "uvc-rknn native runner. Point it at apps/starry/orangepi-5-plus-uvc-rknn/"
            "rknn-yolov8-image (it provides the reused RKNN/UVC/RGA libraries).")
    endif()

    set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")
    set(TARGET_SOC rk3588 CACHE STRING "Rockchip target SoC")

    # Reuse the sibling's 3rdparty resolver (exports LIBRKNNRT/LIBJPEG/LIBRGA +
    # *_INCLUDES, and installs librknnrt.so/librga.so to lib/) and its static
    # image/file/drawing utility libraries.
    add_subdirectory(${TENNIS_RKNN_SHARED_DIR}/3rdparty ${CMAKE_BINARY_DIR}/shared_3rdparty)
    add_subdirectory(${TENNIS_RKNN_SHARED_DIR}/utils ${CMAKE_BINARY_DIR}/shared_utils)

    # Reused, vendored RKNN/UVC sources (compiled into tennis_app directly).
    set(TENNIS_SHARED_SRC
        ${TENNIS_RKNN_SHARED_DIR}/cpp/uvc_capture.cc
        ${TENNIS_RKNN_SHARED_DIR}/cpp/rknpu2/yolov8.cc
        ${TENNIS_RKNN_SHARED_DIR}/cpp/postprocess.cc
    )

    add_executable(tennis_app
        ${TENNIS_CORE_SRC}
        cpp/live.cc
        cpp/perception/camera.cc
        cpp/perception/tennis_detector.cc
        cpp/perception/bucket_detector.cc
        ${TENNIS_SHARED_SRC}
    )

    target_include_directories(tennis_app PRIVATE
        cpp
        ${TENNIS_RKNN_SHARED_DIR}/cpp
        ${TENNIS_RKNN_SHARED_DIR}/cpp/rknpu2
        ${TENNIS_RKNN_SHARED_DIR}/utils
        ${LIBRKNNRT_INCLUDES}
        ${LIBJPEG_INCLUDES}
    )

    target_link_libraries(tennis_app
        imageutils
        fileutils
        ${LIBRKNNRT}
        ${LIBJPEG}
        dl
    )

    # Statically link the C++/GCC runtime so the deployed binary does not depend
    # on the board's libstdc++/libgcc_s versions (the cross toolchain may be newer
    # than the board's runtime). glibc itself stays dynamic and forward-compatible.
    target_link_options(tennis_app PRIVATE -static-libstdc++ -static-libgcc)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(THREADS_PREFER_PTHREAD_FLAG ON)
        find_package(Threads REQUIRED)
        target_link_libraries(tennis_app Threads::Threads)
    endif()

    install(TARGETS tennis_app DESTINATION .)
    install(DIRECTORY model DESTINATION .)
    install(DIRECTORY validation DESTINATION . OPTIONAL)
endif()
