// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "types.h"

namespace tennis {

struct OdometryEstimate {
    bool anchor_set = false;
    bool valid = false;
    double x = 0.0;
    double y = 0.0;
    double heading = 0.0;
    double distance_to_anchor = 0.0;
    double bearing_to_anchor = 0.0;
    int64_t last_sample_ns = 0;
};

class OdometryTracker {
public:
    explicit OdometryTracker(const Config &cfg);

    bool update(int left_rpm, int right_rpm, int64_t sample_ns);
    void reset_anchor();
    OdometryEstimate estimate(int64_t now_ns) const;

private:
    double wheel_speed(int rpm) const;

    double wheel_radius_;
    double wheel_base_;
    int max_rpm_;
    int64_t stale_ns_;
    int64_t max_gap_ns_;

    bool anchor_set_ = false;
    bool have_sample_ = false;
    int64_t last_sample_ns_ = 0;
    double previous_left_speed_ = 0.0;
    double previous_right_speed_ = 0.0;
    double x_ = 0.0;
    double y_ = 0.0;
    double heading_ = 0.0;
};

} // namespace tennis
