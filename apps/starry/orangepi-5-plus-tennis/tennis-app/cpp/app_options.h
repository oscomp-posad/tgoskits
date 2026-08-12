// SPDX-License-Identifier: Apache-2.0
//
// Parsed command-line options, shared by the dry-run simulation (host) and the
// live pipeline / test modes (board). `cfg` carries the tunable thresholds.
#pragma once

#include <string>

#include "types.h"

namespace tennis {

struct Options {
    std::string mode = "dry-run"; // live | test-uvc | test-yolo | test-bucket | dry-run
    std::string model = "model/tennis.rknn";
    std::string label = "model/labels.txt";
    int device = 0;
    int width = 640;
    int height = 480;
    int fps = 30;
    double duration_sec = 60.0;
    bool virtual_actuators = false; // --virtual-actuators: trace backends instead of hardware
    // --- Actuator backends: REAL hardware is the default on board ---
    std::string motor_backend = "uart"; // uart | pwm | virtual
    std::string arm_backend = "uart";   // uart | virtual
    std::string motor_device;           // empty = platform default (UART /dev/ttyS6, PWM chip list)
    std::string arm_device;             // empty = /dev/ttyS3
    int log_every = 1;             // emit per-frame lines every Nth frame (1 = all)
    std::string core_mask = "all"; // NPU core mask for live mode
    // --- Deep profiling (mirrors the sibling uvc-rknn bench) ---
    bool profile = false;          // --profile: collect per-stage timing + emit
                                   // the PROFILE/PIPELINE/COLD_START/RES lines
    int report_interval_sec = 5;   // --report-interval-sec: TENNIS_RES cadence
    std::string profile_csv;       // --profile-csv <path>: one row per frame
    std::string infer_affinity;    // --infer-affinity <cpulist>: pin the inference
                                   // thread (e.g. 4-7 = RK3588 A76 big cores)
    std::string validate_list;     // --validate-list <file>: fixed-image accuracy
                                   // check (one image path per line)
    Config cfg;
};

} // namespace tennis
