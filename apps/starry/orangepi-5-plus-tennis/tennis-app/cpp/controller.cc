// SPDX-License-Identifier: Apache-2.0
#include "controller.h"

#include <cstdio>

#include "time_utils.h"

namespace tennis {

Controller::Controller(const Config &cfg, MotorBackend &motor, ArmBackend &arm,
                       Metrics &metrics, int log_every)
    : sm_(cfg), motor_(motor), arm_(arm), metrics_(metrics),
      log_every_(log_every < 1 ? 1 : log_every) {}

void Controller::apply_motor(const ControlOutput &out) {
    const bool changed =
        out.motor_op != last_op_ ||
        (out.motor_op == MotorOp::Drive &&
         (out.left != last_left_ || out.right != last_right_));
    if (!changed) return;
    switch (out.motor_op) {
    case MotorOp::Drive: motor_.drive(out.left, out.right); break;
    case MotorOp::Brake: motor_.brake(); break;
    case MotorOp::Standby: motor_.standby(); break;
    }
    metrics_.on_motor_command();
    last_op_ = out.motor_op;
    last_left_ = out.left;
    last_right_ = out.right;
}

void Controller::process(const Detection &det) {
    const GameState acting_state = sm_.state();
    const int64_t now = monotonic_ns();
    const double frame_age_ms = ns_to_ms(now - det.capture_ts_ns);

    const bool had_ball = det.mode == PerceptionMode::BALL && det.ball.found;
    const bool had_bucket =
        det.mode == PerceptionMode::BUCKET && det.bucket.found;
    metrics_.on_processed(ns_to_ms(det.detect_ts_ns - det.capture_ts_ns),
                          had_ball, had_bucket);

    const ControlOutput out = sm_.step(det);

    apply_motor(out);

    if (out.arm != ArmAction::None) {
        switch (out.arm) {
        case ArmAction::Grab: arm_.grab(); break;
        case ArmAction::Release: arm_.release(); break;
        case ArmAction::Ready: arm_.ready(); break;
        case ArmAction::None: break;
        }
        metrics_.on_arm_command();
    }

    const int64_t decision_ts = monotonic_ns();
    metrics_.on_command(ns_to_ms(decision_ts - det.capture_ts_ns));

    if ((frame_count_++ % static_cast<unsigned long long>(log_every_)) == 0) {
        const int mleft = out.motor_op == MotorOp::Drive ? out.left : 0;
        const int mright = out.motor_op == MotorOp::Drive ? out.right : 0;
        std::printf("TENNIS_STATE frame=%llu state=%s detections=%d "
                    "bucket_visible=%d frame_age_ms=%.3f\n",
                    static_cast<unsigned long long>(det.seq),
                    to_string(acting_state), had_ball ? 1 : 0,
                    had_bucket ? 1 : 0, frame_age_ms);
        std::printf("TENNIS_CMD frame=%llu state=%s motor_left=%d "
                    "motor_right=%d arm_action=%s capture_ts_ns=%lld "
                    "decision_ts_ns=%lld frame_to_command_ms=%.3f\n",
                    static_cast<unsigned long long>(det.seq),
                    to_string(acting_state), mleft, mright, to_string(out.arm),
                    static_cast<long long>(det.capture_ts_ns),
                    static_cast<long long>(decision_ts),
                    ns_to_ms(decision_ts - det.capture_ts_ns));
    }
}

} // namespace tennis
