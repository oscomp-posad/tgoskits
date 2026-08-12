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
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Tennis-specific sources that are safe to compile on any host (no RKNN/UVC).
set(TENNIS_CORE_SRC
    cpp/main.cc
    cpp/controller.cc
    cpp/odometry.cc
    cpp/odometry_worker.cc
    cpp/state_machine.cc
    cpp/bench/metrics.cc
    cpp/actuator/actuator_factory.cc
    cpp/actuator/trace_motor_backend.cc
    cpp/actuator/trace_arm_backend.cc
)

if(TENNIS_HOST_DRYRUN)
    # --- Host dry-run build: state machine + backends + metrics + synthetic scene.
    add_executable(tennis_app ${TENNIS_CORE_SRC})
    target_compile_definitions(tennis_app PRIVATE TENNIS_HOST_DRYRUN)
    target_include_directories(tennis_app PRIVATE cpp)
    target_compile_options(tennis_app PRIVATE -Wall -Wextra)
    target_link_libraries(tennis_app PRIVATE Threads::Threads)

    include(CTest)
    if(BUILD_TESTING)
        add_executable(tennis_state_machine_test
            cpp/tests/state_machine_test.cc
            cpp/state_machine.cc
            cpp/odometry.cc
        )
        target_include_directories(tennis_state_machine_test PRIVATE cpp)
        target_compile_options(tennis_state_machine_test PRIVATE -Wall -Wextra)
        add_test(NAME tennis_state_machine COMMAND tennis_state_machine_test)

        add_executable(tennis_odometry_test
            cpp/tests/odometry_test.cc
            cpp/odometry.cc
        )
        target_include_directories(tennis_odometry_test PRIVATE cpp)
        target_compile_options(tennis_odometry_test PRIVATE -Wall -Wextra)
        add_test(NAME tennis_odometry COMMAND tennis_odometry_test)

        add_executable(tennis_uart_actuator_test
            cpp/tests/uart_actuator_backend_test.cc
            cpp/actuator/serial_device.cc
            cpp/actuator/uart_motor_backend.cc
            cpp/actuator/uart_arm_backend.cc
        )
        target_include_directories(tennis_uart_actuator_test PRIVATE cpp)
        target_compile_options(tennis_uart_actuator_test PRIVATE -Wall -Wextra)
        target_link_libraries(tennis_uart_actuator_test PRIVATE Threads::Threads)
        find_library(UTIL_LIBRARY util)
        if(UTIL_LIBRARY)
            target_link_libraries(tennis_uart_actuator_test PRIVATE ${UTIL_LIBRARY})
        endif()
        add_test(NAME tennis_uart_actuator COMMAND tennis_uart_actuator_test)
        set_tests_properties(tennis_uart_actuator PROPERTIES TIMEOUT 15)

        add_test(NAME tennis_live_config COMMAND tennis_app
            --config ${CMAKE_CURRENT_SOURCE_DIR}/configs/orangepi5plus-live.conf
            --mode dry-run --virtual-actuators --duration-sec 0.01
            --log-every 1000)
    endif()
else()
    # --- Board build: fully self-contained. All RKNN/UVC/RGA/utils code is now
    # vendored under this app (3rdparty/, utils/, cpp/uvc_capture.cc,
    # cpp/postprocess.cc, cpp/rknpu2/yolov8.cc) -- no dependency on the sibling
    # orangepi-5-plus-uvc-rknn app.
    set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")
    set(TARGET_SOC rk3588 CACHE STRING "Rockchip target SoC")

    # Vendored 3rdparty resolver (exports LIBRKNNRT/LIBJPEG/LIBRGA + *_INCLUDES,
    # installs librknnrt.so/librga.so to lib/) and the static image/file/drawing
    # utility libraries. 3rdparty must precede utils (utils links ${LIBRGA}).
    add_subdirectory(3rdparty ${CMAKE_BINARY_DIR}/vendor_3rdparty)
    add_subdirectory(utils ${CMAKE_BINARY_DIR}/vendor_utils)

    # Vendored RKNN/UVC sources (compiled into tennis_app directly).
    set(TENNIS_VENDORED_SRC
        cpp/uvc_capture.cc
        cpp/rknpu2/yolov8.cc
        cpp/postprocess.cc
    )

    # Vendored librockchip_mpp (RK3588 JPU MJPEG decode via /dev/mpp_service).
    set(LIBMPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/mpp)
    set(LIBMPP_INCLUDES ${LIBMPP_DIR}/include)
    find_library(LIBMPP NAMES rockchip_mpp PATHS ${LIBMPP_DIR}/lib NO_DEFAULT_PATH)
    if(NOT LIBMPP)
        message(FATAL_ERROR "librockchip_mpp not found in ${LIBMPP_DIR}/lib")
    endif()

    add_executable(tennis_app
        ${TENNIS_CORE_SRC}
        cpp/live.cc
        cpp/perception/camera.cc
        cpp/perception/tennis_detector.cc
        cpp/perception/jpu_decoder.cc
        cpp/perception/bucket_detector.cc
        cpp/actuator/serial_device.cc
        cpp/actuator/pwm_motor_backend.cc
        cpp/actuator/uart_motor_backend.cc
        cpp/actuator/uart_arm_backend.cc
        ${TENNIS_VENDORED_SRC}
    )

    target_include_directories(tennis_app PRIVATE
        cpp
        cpp/rknpu2
        utils
        ${LIBRKNNRT_INCLUDES}
        ${LIBJPEG_INCLUDES}
        ${LIBRGA_INCLUDES}
        ${LIBMPP_INCLUDES}
    )

    # LIBRGA is linked explicitly: the detector calls librga (imcvtcolor /
    # importbuffer_fd) directly for the zero-copy YUYV->RGB preprocess, not only
    # transitively through imageutils.
    target_link_libraries(tennis_app
        imageutils
        fileutils
        ${LIBRKNNRT}
        ${LIBJPEG}
        ${LIBRGA}
        ${LIBMPP}
        Threads::Threads
        dl
    )

    # Statically link the C++/GCC runtime so the deployed binary does not depend
    # on the board's libstdc++/libgcc_s versions (the cross toolchain may be newer
    # than the board's runtime). glibc itself stays dynamic and forward-compatible.
    target_link_options(tennis_app PRIVATE -static-libstdc++ -static-libgcc)

    install(TARGETS tennis_app DESTINATION .)
    # Deploy librockchip_mpp alongside the binary (RPATH is $ORIGIN/lib).
    file(GLOB LIBMPP_SOFILES ${LIBMPP_DIR}/lib/librockchip_mpp.so*)
    install(FILES ${LIBMPP_SOFILES} DESTINATION lib)
    install(DIRECTORY model DESTINATION .)
    install(DIRECTORY configs DESTINATION .)
    install(DIRECTORY validation DESTINATION . OPTIONAL)
endif()
