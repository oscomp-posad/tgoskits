// SPDX-License-Identifier: Apache-2.0
#include "controller.h"

#include <cstdio>

#include "time_utils.h"

namespace tennis {

Controller::Controller(const Config &cfg, MotorBackend &motor, ArmBackend &arm,
                       Metrics &metrics, int log_every)
    : sm_(cfg), odometry_(cfg, motor), motor_(motor), arm_(arm),
      metrics_(metrics), log_every_(log_every < 1 ? 1 : log_every) {
    sm_.set_odometry(odometry_.estimate(monotonic_ns()));
}

void Controller::refresh_odometry(int64_t now_ns) {
    sm_.set_odometry(odometry_.estimate(now_ns));
}

bool Controller::apply_motor(const ControlOutput &out) {
    const bool changed =
        out.motor_op != last_op_ ||
        (out.motor_op == MotorOp::Drive &&
         (out.left != last_left_ || out.right != last_right_));
    if (!changed) return true;
    bool succeeded = false;
    switch (out.motor_op) {
    case MotorOp::Drive: succeeded = motor_.drive(out.left, out.right); break;
    case MotorOp::Brake: succeeded = motor_.brake(); break;
    case MotorOp::Standby: succeeded = motor_.standby(); break;
    }
    if (!succeeded) return false;
    metrics_.on_motor_command();
    last_op_ = out.motor_op;
    last_left_ = out.left;
    last_right_ = out.right;
    return true;
}

bool Controller::process(const Detection &det) {
    const GameState acting_state = sm_.state();
    int64_t now = monotonic_ns();
    refresh_odometry(now);
    now = monotonic_ns();
    const double frame_age_ms = ns_to_ms(now - det.capture_ts_ns);

    const bool had_ball = det.mode == PerceptionMode::BALL && det.ball.found;
    const bool had_bucket =
        det.mode == PerceptionMode::BUCKET && det.bucket.found;
    metrics_.on_processed(ns_to_ms(det.detect_ts_ns - det.capture_ts_ns),
                          had_ball, had_bucket);

    const ControlOutput out = sm_.step(det, now);
    if (!dispatch(out)) return false;

    const int64_t decision_ts = monotonic_ns();
    metrics_.on_command(ns_to_ms(decision_ts - det.capture_ts_ns));

    if ((frame_count_++ % static_cast<unsigned long long>(log_every_)) == 0) {
        const int mleft = out.motor_op == MotorOp::Drive ? out.left : 0;
        const int mright = out.motor_op == MotorOp::Drive ? out.right : 0;
        const OdometryEstimate odom = odometry_.estimate(decision_ts);
        std::printf("TENNIS_STATE frame=%llu state=%s detections=%d "
                    "bucket_visible=%d ball_area=%.4f ball_cx=%.1f "
                    "odom_valid=%d odom_x=%.3f odom_y=%.3f "
                    "odom_heading=%.3f odom_distance=%.3f frame_age_ms=%.3f\n",
                    static_cast<unsigned long long>(det.seq),
                    to_string(acting_state), had_ball ? 1 : 0,
                    had_bucket ? 1 : 0,
                    had_ball ? det.ball.area_ratio : 0.0f,
                    had_ball ? det.ball.cx : 0.0f, odom.valid ? 1 : 0,
                    odom.x, odom.y, odom.heading, odom.distance_to_anchor,
                    frame_age_ms);
        std::printf("TENNIS_CMD frame=%llu state=%s motor_left=%d "
                    "motor_right=%d arm_action=%s capture_ts_ns=%lld "
                    "decision_ts_ns=%lld frame_to_command_ms=%.3f\n",
                    static_cast<unsigned long long>(det.seq),
                    to_string(acting_state), mleft, mright, to_string(out.arm),
                    static_cast<long long>(det.capture_ts_ns),
                    static_cast<long long>(decision_ts),
                    ns_to_ms(decision_ts - det.capture_ts_ns));
    }
    return true;
}

bool Controller::tick(int64_t now_ns) {
    refresh_odometry(now_ns);
    const auto output = sm_.tick(now_ns);
    return !output || dispatch(*output);
}

bool Controller::dispatch(const ControlOutput &out) {
    if (!apply_motor(out)) {
        std::fprintf(stderr, "TENNIS_ERROR motor command failed\n");
        return false;
    }

    if (out.arm == ArmAction::None) return true;
    bool succeeded = false;
    switch (out.arm) {
    case ArmAction::Grab: {
        const GrabResult result = arm_.grab();
        if (result == GrabResult::Empty) {
            metrics_.on_arm_command();
            sm_.on_grab_empty();
            std::fprintf(stderr, "TENNIS_WARN arm grab empty; resuming ball search\n");
            return true;
        }
        succeeded = result == GrabResult::Captured;
        break;
    }
    case ArmAction::Release: succeeded = arm_.release(); break;
    case ArmAction::Ready: succeeded = arm_.ready(); break;
    case ArmAction::None: succeeded = true; break;
    }
    if (!succeeded) {
        std::fprintf(stderr, "TENNIS_ERROR arm command failed\n");
        return false;
    }
    metrics_.on_arm_command();
    if (out.reset_odometry) {
        odometry_.reset_anchor();
        sm_.set_odometry(odometry_.estimate(monotonic_ns()));
        std::printf("TENNIS_ODOMETRY anchor_reset=1\n");
    }
    return true;
}

} // namespace tennis
