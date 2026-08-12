// SPDX-License-Identifier: Apache-2.0
//
// Game state machine + differential-drive steering. Pure logic: it consumes a
// perception Detection and returns the desired motor/arm command. It performs
// no I/O and no blocking sleeps (the reference robot busy-waited for hundreds
// of ms inside brake/grab/deposit; here those are brief non-blocking states),
// which keeps end-to-end frame->command latency equal to pure compute.
//
// This is a clean reimplementation of the aka-rk3588 control flow (that repo
// carries no license); constants live in Config and are re-derived, not copied.
#pragma once

#include "actuator/arm_backend.h"
#include "types.h"

namespace tennis {

enum class MotorOp { Drive, Brake, Standby };

struct ControlOutput {
    MotorOp motor_op = MotorOp::Standby;
    int left = 0;  // valid when motor_op == Drive
    int right = 0; // valid when motor_op == Drive
    ArmAction arm = ArmAction::None;
};

class StateMachine {
public:
    explicit StateMachine(const Config &cfg);

    // Run one control step against the latest detection; updates internal state.
    ControlOutput step(const Detection &det);

    // Detector the perception stage should run for the next frame.
    PerceptionMode perception_mode() const;

    GameState state() const { return state_; }

private:
    ControlOutput chase_ball(const BallObs &ball);
    ControlOutput grab();
    ControlOutput find_bucket(const BucketObs &bucket);
    ControlOutput approach_bucket(const BucketObs &bucket);
    ControlOutput deposit();

    int base_speed(float area_ratio) const;
    void enter(GameState s);

    Config cfg_;
    int half_w_;
    GameState state_ = GameState::CHASE_BALL;

    // CHASE_BALL bookkeeping.
    int frames_since_seen_ = 1 << 20;
    int last_offset_ = 0;
    int scan_dir_ = 1;
    int scan_counter_ = 0;
    int stop_confirm_ = 0;
    int align_frames_ = 0;
    int align_off_min_ = 0;
    int align_off_max_ = 0;

    // Transient-state settle counters.
    int grab_frames_ = 0;
    int deposit_frames_ = 0;

    // Bucket bookkeeping.
    int bucket_confirm_ = 0;
    int bucket_lost_ = 0;
};

} // namespace tennis
