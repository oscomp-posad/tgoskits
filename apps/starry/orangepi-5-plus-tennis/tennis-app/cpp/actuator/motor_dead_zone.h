// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdlib>

namespace tennis {

struct WheelCommand {
    int left;
    int right;
};

// Map each non-zero wheel independently out of the physical dead zone. Higher
// level control code is responsible for composing base speed and steering bias.
inline WheelCommand apply_motor_dead_zone(int left, int right, int minimum) {
    left = std::clamp(left, -100, 100);
    right = std::clamp(right, -100, 100);
    minimum = std::clamp(minimum, 1, 100);

    const auto map_wheel = [minimum](int speed) {
        if (speed == 0) return 0;
        return (speed > 0 ? 1 : -1) *
               std::max(std::abs(speed), minimum);
    };
    return {map_wheel(left), map_wheel(right)};
}

} // namespace tennis
