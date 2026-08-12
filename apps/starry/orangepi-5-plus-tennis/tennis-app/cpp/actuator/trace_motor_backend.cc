// SPDX-License-Identifier: Apache-2.0
#include "motor_backend.h"

#include <cstdio>

namespace tennis {

// The control layer de-duplicates identical consecutive commands, so these
// lines appear only when the motor command actually changes -- structured,
// machine-readable, and not hot-path spam.
bool TraceMotorBackend::drive(int left, int right) {
    std::printf("TENNIS_MOTOR drive left=%d right=%d\n", left, right);
    return true;
}

bool TraceMotorBackend::brake() {
    std::printf("TENNIS_MOTOR brake\n");
    return true;
}

bool TraceMotorBackend::standby() {
    std::printf("TENNIS_MOTOR standby\n");
    return true;
}

} // namespace tennis
