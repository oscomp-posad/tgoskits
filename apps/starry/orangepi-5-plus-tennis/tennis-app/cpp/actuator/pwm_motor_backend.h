// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <string>

#include "motor_backend.h"

namespace tennis {

class PwmMotorBackend final : public MotorBackend {
public:
    explicit PwmMotorBackend(const std::string &spec);
    ~PwmMotorBackend() override;

    bool ready() const { return ready_; }
    bool drive(int left, int right) override;
    bool brake() override;
    bool standby() override;

private:
    struct Output {
        std::string chip;
        std::string channel;
    };
    enum Index { LeftForward, LeftBackward, RightForward, RightBackward };

    bool parse(const std::string &spec);
    bool initialize(Output &output);
    bool set_output(Index index, bool enable, int speed);
    bool set_pair(Index forward, Index backward, int speed);

    std::array<Output, 4> outputs_{};
    bool ready_ = false;
};

} // namespace tennis
