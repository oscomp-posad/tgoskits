// SPDX-License-Identifier: Apache-2.0
//
// Game state machine + differential-drive steering. Pure logic: it consumes a
// perception Detection and returns the desired motor/arm command. It performs
// no I/O and no blocking sleeps. Calibrated brake intervals are monotonic-time
// phases advanced by step()/tick(), independent of camera FPS.
//
// This is a clean reimplementation of the aka-rk3588 control flow (that repo
// carries no license); constants live in Config and are re-derived, not copied.
#pragma once

#include <optional>

#include "actuator/arm_backend.h"
#include "odometry.h"
#include "types.h"

namespace tennis {

enum class MotorOp { Drive, Brake, Standby };

struct ControlOutput {
    MotorOp motor_op = MotorOp::Standby;
    int left = 0;  // valid when motor_op == Drive
    int right = 0; // valid when motor_op == Drive
    ArmAction arm = ArmAction::None;
    bool reset_odometry = false;
};

class StateMachine {
public:
    explicit StateMachine(const Config &cfg);

    // Run one control step against the latest detection; updates internal state.
    ControlOutput step(const Detection &det, int64_t now_ns);

    // Advance elapsed-time phases without consuming another detection. This
    // keeps motor deadlines independent of camera FPS and temporary frame gaps.
    std::optional<ControlOutput> tick(int64_t now_ns);

    // Detector the perception stage should run for the next frame.
    PerceptionMode perception_mode() const;

    void set_odometry(const OdometryEstimate &estimate) {
        odometry_ = estimate;
    }

    GameState state() const { return state_; }

    // A verified empty grasp resumes ball search instead of advancing to the
    // bucket flow. Transport failures are handled by Controller separately.
    void on_grab_empty();

private:
    enum TimedPhase {
        Normal,
        BrakeForGrab,
        BrakeForDeposit,
        ReverseAfterDeposit,
    };

    ControlOutput chase_ball(const BallObs &ball, int64_t now_ns);
    ControlOutput grab(int64_t now_ns);
    ControlOutput return_to_bucket(const BucketObs &bucket, int64_t now_ns);
    ControlOutput find_bucket(const BucketObs &bucket, int64_t now_ns);
    ControlOutput approach_bucket(const BucketObs &bucket, int64_t now_ns);
    ControlOutput deposit(int64_t now_ns);

    void enter(GameState s);
    void reset_search_rotation();
    int search_rotation_direction(int initial_direction, int speed,
                                  int64_t now_ns);
    bool run_timed_phase(int64_t now_ns, ControlOutput &out);
    void start_timed_phase(TimedPhase phase, int64_t now_ns, int duration_ms);

    Config cfg_;
    int half_w_;
    GameState state_ = GameState::CHASE_BALL;

    // CHASE_BALL bookkeeping.
    int last_offset_ = 0;
    int stop_confirm_ = 0;
    // Time-based back-and-forth search sweep (Ivans' refine-search).
    bool search_rotation_active_ = false;
    int search_rotation_direction_ = 1;
    int64_t search_rotation_deadline_ns_ = 0;
    // Lost-frame coast: hold the last pursuit command across brief detection
    // dropouts (up to cfg_.chase_lost_coast_frames) instead of immediately
    // scanning, so a single missed frame does not flip pursuit into a spin.
    int frames_since_ball_ = 0;
    ControlOutput last_chase_out_{};
    bool last_chase_valid_ = false;

    TimedPhase timed_phase_ = Normal;
    int64_t timed_deadline_ns_ = 0;

    // GRAB/DEPOSIT issue their arm command once, then wait on an elapsed-time
    // deadline rather than a frame count that changes with camera load.
    bool state_action_started_ = false;
    int64_t state_deadline_ns_ = 0;

    OdometryEstimate odometry_;
    int64_t return_start_ns_ = 0;

    // Bucket bookkeeping.
    int bucket_confirm_ = 0;
    int bucket_lost_ = 0;
};

} // namespace tennis
