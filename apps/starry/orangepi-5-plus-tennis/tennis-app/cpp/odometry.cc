// SPDX-License-Identifier: Apache-2.0
#include "odometry.h"

#include <algorithm>
#include <cmath>

namespace tennis {
namespace {

constexpr double kPi = 3.14159265358979323846;

double normalize_angle(double angle) {
    return std::remainder(angle, 2.0 * kPi);
}

} // namespace

OdometryTracker::OdometryTracker(const Config &cfg)
    : wheel_radius_(cfg.odometry_wheel_radius_m),
      wheel_base_(cfg.odometry_wheel_base_m),
      max_rpm_(cfg.odometry_max_rpm),
      stale_ns_(static_cast<int64_t>(cfg.odometry_stale_ms) * 1000000),
      max_gap_ns_(static_cast<int64_t>(cfg.odometry_max_gap_ms) * 1000000) {}

double OdometryTracker::wheel_speed(int rpm) const {
    return static_cast<double>(rpm) / 60.0 * 2.0 * kPi * wheel_radius_;
}

bool OdometryTracker::update(int left_rpm, int right_rpm, int64_t sample_ns) {
    if (sample_ns <= 0 || std::abs(left_rpm) > max_rpm_ ||
        std::abs(right_rpm) > max_rpm_)
        return false;

    const double left_speed = wheel_speed(left_rpm);
    const double right_speed = wheel_speed(right_rpm);
    if (!have_sample_) {
        have_sample_ = true;
        last_sample_ns_ = sample_ns;
        previous_left_speed_ = left_speed;
        previous_right_speed_ = right_speed;
        return true;
    }
    if (sample_ns <= last_sample_ns_) return false;

    const int64_t elapsed_ns = sample_ns - last_sample_ns_;
    if (elapsed_ns <= max_gap_ns_) {
        const double dt = static_cast<double>(elapsed_ns) / 1.0e9;
        const double integrated_left =
            0.5 * (previous_left_speed_ + left_speed);
        const double integrated_right =
            0.5 * (previous_right_speed_ + right_speed);
        const double linear = 0.5 * (integrated_left + integrated_right);
        const double angular =
            (integrated_right - integrated_left) / wheel_base_;
        const double delta_heading = angular * dt;
        const double midpoint = heading_ + 0.5 * delta_heading;
        x_ += linear * dt * std::cos(midpoint);
        y_ += linear * dt * std::sin(midpoint);
        heading_ = normalize_angle(heading_ + delta_heading);
    }

    last_sample_ns_ = sample_ns;
    previous_left_speed_ = left_speed;
    previous_right_speed_ = right_speed;
    return true;
}

void OdometryTracker::reset_anchor() {
    anchor_set_ = true;
    x_ = 0.0;
    y_ = 0.0;
    heading_ = 0.0;
}

OdometryEstimate OdometryTracker::estimate(int64_t now_ns) const {
    OdometryEstimate result;
    result.anchor_set = anchor_set_;
    result.x = x_;
    result.y = y_;
    result.heading = heading_;
    result.distance_to_anchor = std::hypot(x_, y_);
    result.bearing_to_anchor = std::atan2(-y_, -x_);
    result.last_sample_ns = last_sample_ns_;
    result.valid = anchor_set_ && have_sample_ && now_ns >= last_sample_ns_ &&
                   now_ns - last_sample_ns_ <= stale_ns_;
    return result;
}

} // namespace tennis
