// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>

#include "arm_backend.h"
#include "motor_backend.h"

namespace tennis {

struct Options;

struct Actuators {
    std::unique_ptr<MotorBackend> motor;
    std::unique_ptr<ArmBackend> arm;
};

bool make_actuators(const Options &options, Actuators &actuators);
bool uses_virtual_actuators(const Options &options);

} // namespace tennis
