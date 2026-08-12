// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include "arm_backend.h"
#include "serial_device.h"

namespace tennis {

class UartArmBackend final : public ArmBackend {
public:
    explicit UartArmBackend(const std::string &device);

    bool is_ready() const { return ready_; }
    GrabResult grab() override;
    bool release() override;
    bool ready() override;

private:
    bool set_angle(int servo, float angle, int time_ms = 500);
    bool set_pose(float servo0, float servo1, float servo2,
                  int time_ms = 500);
    bool send(const std::string &command);
    bool read_position(int servo, int &pulse);

    SerialDevice serial_;
    bool ready_ = false;
};

} // namespace tennis
