// SPDX-License-Identifier: Apache-2.0
#include "state_machine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace tennis {

namespace {

constexpr int64_t kNsPerMs = 1000000;
constexpr double kPi = 3.14159265358979323846;
constexpr double kEstimatedDegreesPerSecondPerSpeedUnit = 2.0;

double normalize_angle(double angle) {
    return std::remainder(angle, 2.0 * kPi);
}

} // namespace

const char *to_string(GameState s) {
    switch (s) {
    case GameState::CHASE_BALL: return "CHASE_BALL";
    case GameState::GRAB: return "GRAB";
    case GameState::RETURN_TO_BUCKET: return "RETURN_TO_BUCKET";
    case GameState::FIND_BUCKET: return "FIND_BUCKET";
    case GameState::APPROACH_BUCKET: return "APPROACH_BUCKET";
    case GameState::DEPOSIT: return "DEPOSIT";
    }
    return "CHASE_BALL";
}

const char *to_string(PerceptionMode m) {
    return m == PerceptionMode::BALL ? "BALL" : "BUCKET";
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

StateMachine::StateMachine(const Config &cfg)
    : cfg_(cfg), half_w_(cfg.frame_w / 2) {}

PerceptionMode StateMachine::perception_mode() const {
    switch (state_) {
    case GameState::CHASE_BALL:
    case GameState::GRAB:
        return PerceptionMode::BALL;
    default:
        return PerceptionMode::BUCKET;
    }
}

void StateMachine::enter(GameState s) {
    state_ = s;
    // Reset per-state bookkeeping on entry.
    stop_confirm_ = 0;
    frames_since_ball_ = 0;
    last_chase_valid_ = false;
    state_action_started_ = false;
    state_deadline_ns_ = 0;
    bucket_confirm_ = 0;
    bucket_lost_ = 0;
    reset_search_rotation();
}

void StateMachine::reset_search_rotation() {
    search_rotation_active_ = false;
    search_rotation_deadline_ns_ = 0;
}

int StateMachine::search_rotation_direction(int initial_direction, int speed,
                                            int64_t now_ns) {
    // Estimate rotation from elapsed time and the requested wheel speed only;
    // no encoder sample, odometry estimate, or odometry configuration is used.
    const double duration_ms =
        cfg_.search_reverse_turns * 360.0 * 1000.0 /
        (std::max(speed, 1) * kEstimatedDegreesPerSecondPerSpeedUnit);
    const int64_t duration_ns =
        std::max<int64_t>(1, static_cast<int64_t>(std::llround(duration_ms))) *
        kNsPerMs;
    if (!search_rotation_active_) {
        search_rotation_active_ = true;
        search_rotation_direction_ = initial_direction >= 0 ? 1 : -1;
        search_rotation_deadline_ns_ = now_ns + duration_ns;
    } else {
        while (now_ns >= search_rotation_deadline_ns_) {
            search_rotation_direction_ = -search_rotation_direction_;
            search_rotation_deadline_ns_ += duration_ns;
        }
    }
    return search_rotation_direction_;
}

void StateMachine::on_grab_empty() {
    if (state_ == GameState::GRAB) enter(GameState::CHASE_BALL);
}

void StateMachine::start_timed_phase(TimedPhase phase, int64_t now_ns,
                                     int duration_ms) {
    timed_phase_ = phase;
    timed_deadline_ns_ = now_ns +
                         static_cast<int64_t>(std::max(duration_ms, 0)) *
                             kNsPerMs;
}

bool StateMachine::run_timed_phase(int64_t now_ns, ControlOutput &out) {
    switch (timed_phase_) {
    case Normal: return false;
    case BrakeForGrab:
        if (now_ns < timed_deadline_ns_) {
            out.motor_op = MotorOp::Brake;
            return true;
        }
        timed_phase_ = Normal;
        enter(GameState::GRAB);
        out = grab(now_ns);
        return true;
    case BrakeForDeposit:
        if (now_ns < timed_deadline_ns_) {
            out.motor_op = MotorOp::Brake;
            return true;
        }
        timed_phase_ = Normal;
        enter(GameState::DEPOSIT);
        out = deposit(now_ns);
        return true;
    case ReverseAfterDeposit:
        if (now_ns < timed_deadline_ns_) {
            out.motor_op = MotorOp::Drive;
            out.left = -cfg_.deposit_reverse_speed;
            out.right = -cfg_.deposit_reverse_speed;
            return true;
        }
        timed_phase_ = Normal;
        enter(GameState::CHASE_BALL);
        out.motor_op = MotorOp::Standby;
        out.reset_odometry = true;
        return true;
    }
    return false;
}

ControlOutput StateMachine::step(const Detection &det, int64_t now_ns) {
    ControlOutput timed_output;
    if (run_timed_phase(now_ns, timed_output)) return timed_output;

    switch (state_) {
    case GameState::CHASE_BALL: {
        // Coast the last pursuit command through a brief detection dropout
        // before letting chase_ball fall back to the one-way scan. A single
        // missed frame must not flip a forward pursuit into an opposing-wheel
        // spin (and last_offset_ is stale on a lost frame, so the scan could
        // even pivot the wrong way). Only coasts after the ball has been seen
        // this episode; chase_ball stays the sole owner of the active steering.
        if (!det.ball.found && last_chase_valid_ &&
            frames_since_ball_ < cfg_.chase_lost_coast_frames) {
            ++frames_since_ball_;
            return last_chase_out_;
        }
        const ControlOutput out = chase_ball(det.ball, now_ns);
        if (det.ball.found) {
            last_chase_out_ = out;
            last_chase_valid_ = true;
            frames_since_ball_ = 0;
        }
        return out;
    }
    case GameState::GRAB: return grab(now_ns);
    case GameState::RETURN_TO_BUCKET:
        return return_to_bucket(det.bucket, now_ns);
    case GameState::FIND_BUCKET: return find_bucket(det.bucket, now_ns);
    case GameState::APPROACH_BUCKET:
        return approach_bucket(det.bucket, now_ns);
    case GameState::DEPOSIT: return deposit(now_ns);
    }
    return {};
}

std::optional<ControlOutput> StateMachine::tick(int64_t now_ns) {
    ControlOutput out;
    if (run_timed_phase(now_ns, out)) return out;
    if (state_ == GameState::GRAB) return grab(now_ns);
    if (state_ == GameState::DEPOSIT) return deposit(now_ns);
    return std::nullopt;
}

ControlOutput StateMachine::chase_ball(const BallObs &ball, int64_t now_ns) {
    ControlOutput out;

    if (!ball.found) {
        stop_confirm_ = 0;
        out.motor_op = MotorOp::Drive;
        const int dir = search_rotation_direction(
            last_offset_ >= 0 ? 1 : -1, cfg_.search_pivot_spd, now_ns);
        out.left = dir * cfg_.search_pivot_spd;
        out.right = -dir * cfg_.search_pivot_spd;
        return out;
    }

    reset_search_rotation();

    const float area = ball.area_ratio;
    const int offset = static_cast<int>(ball.cx) - half_w_;
    last_offset_ = offset;
    const int stop_off = offset - cfg_.stop_center_offset;

    // Too close: back straight up before trying to align again.
    if (area >= cfg_.area_reverse) {
        stop_confirm_ = 0;
        out.motor_op = MotorOp::Drive;
        out.left = -cfg_.reverse_speed;
        out.right = -cfg_.reverse_speed;
        return out;
    }

    // Drive toward the ball while steering in proportion to its horizontal
    // error. When the base speed is at the physical floor, keep the inside
    // wheel at that floor and put the complete wheel-speed difference on the
    // outside wheel.
    if (area < cfg_.area_stop) {
        stop_confirm_ = 0;
        const int speed = area < cfg_.area_far ? cfg_.chase_far_spd
                                               : cfg_.chase_forward_spd;
        int bias = (std::abs(stop_off) <= cfg_.stop_center_zone)
                       ? 0
                       : static_cast<int>(std::lround(
                             cfg_.chase_turn_k * stop_off /
                             static_cast<float>(half_w_)));
        bias = clampi(bias, -cfg_.chase_max_bias, cfg_.chase_max_bias);
        out.motor_op = MotorOp::Drive;
        if (speed - std::abs(bias) < cfg_.motor_min_speed) {
            out.left = clampi(
                cfg_.motor_min_speed + std::max(2 * bias, 0), 0, 100);
            out.right = clampi(
                cfg_.motor_min_speed + std::max(-2 * bias, 0), 0, 100);
        } else {
            out.left = clampi(speed + bias, 0, 100);
            out.right = clampi(speed - bias, 0, 100);
        }
        return out;
    }

    // A close ball must still be aligned before the grab is confirmed. Pivot
    // in place at a fixed executable speed so alignment adds no forward motion.
    if (std::abs(stop_off) > cfg_.stop_center_zone) {
        stop_confirm_ = 0;
        const int direction = stop_off > 0 ? 1 : -1;
        out.motor_op = MotorOp::Drive;
        out.left = direction * cfg_.chase_close_pivot_spd;
        out.right = -out.left;
        return out;
    }

    // Close and aligned: brake, confirm on fresh frames, then move the arm.
    if (++stop_confirm_ >= cfg_.stop_confirm_cnt) {
        start_timed_phase(BrakeForGrab, now_ns, cfg_.brake_hold_ms);
    }
    out.motor_op = MotorOp::Brake;
    return out;
}

ControlOutput StateMachine::grab(int64_t now_ns) {
    ControlOutput out;
    out.motor_op = MotorOp::Standby;
    if (!state_action_started_) {
        out.arm = ArmAction::Grab;
        state_action_started_ = true;
        state_deadline_ns_ = now_ns +
                             static_cast<int64_t>(
                                 std::max(cfg_.grab_settle_ms, 0)) *
                                 kNsPerMs;
    } else if (now_ns >= state_deadline_ns_) {
        if (odometry_.valid) {
            enter(GameState::RETURN_TO_BUCKET);
            return_start_ns_ = now_ns;
        } else {
            enter(GameState::FIND_BUCKET);
        }
    }
    return out;
}

ControlOutput StateMachine::return_to_bucket(const BucketObs &bucket,
                                             int64_t now_ns) {
    ControlOutput out;
    out.motor_op = MotorOp::Brake;

    // A visual bucket always takes over before any odometry command.
    if (bucket.found) {
        enter(GameState::FIND_BUCKET);
        return out;
    }
    const double timeout_ns =
        static_cast<double>(cfg_.return_timeout_ms) * kNsPerMs;
    if (!odometry_.valid ||
        now_ns - return_start_ns_ > static_cast<int64_t>(timeout_ns) ||
        odometry_.distance_to_anchor <= cfg_.return_stop_radius_m ||
        odometry_.distance_to_anchor >= cfg_.return_max_distance_m) {
        enter(GameState::FIND_BUCKET);
        return out;
    }

    const double tolerance = cfg_.return_heading_tolerance_deg * kPi / 180.0;
    const double error =
        normalize_angle(odometry_.bearing_to_anchor - odometry_.heading);
    if (std::abs(error) > tolerance) {
        // Positive mathematical heading is produced by right-wheel-forward,
        // left-wheel-reverse, hence the negative sign for the wheel command.
        const int dir = error > 0.0 ? -1 : 1;
        out.motor_op = MotorOp::Drive;
        out.left = dir * cfg_.return_pivot_spd;
        out.right = -out.left;
    } else {
        out.motor_op = MotorOp::Drive;
        out.left = cfg_.return_forward_spd;
        out.right = cfg_.return_forward_spd;
    }
    return out;
}

ControlOutput StateMachine::find_bucket(const BucketObs &bucket,
                                        int64_t now_ns) {
    ControlOutput out;
    if (bucket.found) {
        reset_search_rotation();
        bucket_lost_ = 0;
        if (++bucket_confirm_ >= cfg_.bucket_confirm_cnt) {
            enter(GameState::APPROACH_BUCKET);
        }
        out.motor_op = MotorOp::Standby; // hold steady while confirming
    } else {
        bucket_confirm_ = 0;
        out.motor_op = MotorOp::Drive; // rotate-in-place search
        const int dir = search_rotation_direction(
            1, cfg_.bucket_search_spd, now_ns);
        out.left = dir * cfg_.bucket_search_spd;
        out.right = -dir * cfg_.bucket_search_spd;
    }
    return out;
}

ControlOutput StateMachine::approach_bucket(const BucketObs &bucket,
                                            int64_t now_ns) {
    ControlOutput out;
    if (!bucket.found) {
        if (++bucket_lost_ > cfg_.bucket_lost_frames) {
            enter(GameState::FIND_BUCKET);
        }
        out.motor_op = MotorOp::Standby;
        return out;
    }
    bucket_lost_ = 0;

    const float area = bucket.area_ratio;
    if (area >= cfg_.bucket_area_deposit) {
        start_timed_phase(BrakeForDeposit, now_ns, cfg_.brake_hold_ms);
        out.motor_op = MotorOp::Brake;
        return out;
    }

    int bk_off = static_cast<int>(bucket.cx) - half_w_;
    int bias = (std::abs(bk_off) <= cfg_.center_dead_zone)
                   ? 0
                   : static_cast<int>(std::lround(
                         cfg_.bucket_k_turn * bk_off /
                         static_cast<float>(half_w_)));
    bias = clampi(bias, -cfg_.bucket_max_bias, cfg_.bucket_max_bias);
    int spd = (area >= cfg_.bucket_area_brake) ? cfg_.bucket_brake_spd
                                               : cfg_.bucket_approach_spd;
    out.motor_op = MotorOp::Drive;
    if (spd < cfg_.motor_min_speed) {
        // Below the executable translation floor, subtracting the steering
        // bias would leave one wheel in the physical dead zone. Start both
        // wheels at the floor and put twice the original bias on only the
        // outside wheel. This preserves the requested wheel-speed difference.
        out.left = clampi(cfg_.motor_min_speed + std::max(2 * bias, 0), 0, 100);
        out.right =
            clampi(cfg_.motor_min_speed + std::max(-2 * bias, 0), 0, 100);
    } else {
        out.left = clampi(spd + bias, -100, 100);
        out.right = clampi(spd - bias, -100, 100);
    }
    return out;
}

ControlOutput StateMachine::deposit(int64_t now_ns) {
    ControlOutput out;
    out.motor_op = MotorOp::Standby;
    if (!state_action_started_) {
        out.arm = ArmAction::Release;
        state_action_started_ = true;
        state_deadline_ns_ = now_ns +
                             static_cast<int64_t>(
                                 std::max(cfg_.release_settle_ms, 0)) *
                                 kNsPerMs;
    } else if (now_ns >= state_deadline_ns_) {
        if (cfg_.deposit_reverse_ms > 0) {
            start_timed_phase(ReverseAfterDeposit, now_ns,
                              cfg_.deposit_reverse_ms);
            out.motor_op = MotorOp::Drive;
            out.left = -cfg_.deposit_reverse_speed;
            out.right = -cfg_.deposit_reverse_speed;
        } else {
            enter(GameState::CHASE_BALL);
            out.reset_odometry = true;
        }
    }
    return out;
}

} // namespace tennis
