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
    bool virtual_actuators = true; // the only supported actuator path for now
    int log_every = 1;             // emit per-frame lines every Nth frame (1 = all)
    std::string core_mask = "all"; // NPU core mask for live mode
    Config cfg;
};

} // namespace tennis
