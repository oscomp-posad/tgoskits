// SPDX-License-Identifier: Apache-2.0
//
// Per-run control loop: turns each perception Detection into a motor/arm command
// via the state machine, de-duplicates identical motor commands (so the backend
// trace is not hot-path spam), records benchmark samples, and emits the
// per-frame TENNIS_STATE / TENNIS_CMD lines. Shared by the dry-run simulation
// (host) and the live pipeline (board).
#pragma once

#include "actuator/arm_backend.h"
#include "actuator/motor_backend.h"
#include "bench/metrics.h"
#include "odometry_worker.h"
#include "state_machine.h"
#include "types.h"

namespace tennis {

class Controller {
public:
    Controller(const Config &cfg, MotorBackend &motor, ArmBackend &arm,
               Metrics &metrics, int log_every);

    PerceptionMode perception_mode() const { return sm_.perception_mode(); }
    GameState state() const { return sm_.state(); }

    bool process(const Detection &det);
    bool tick(int64_t now_ns);

private:
    bool apply_motor(const ControlOutput &out);
    bool dispatch(const ControlOutput &out);
    void refresh_odometry(int64_t now_ns);

    StateMachine sm_;
    OdometryWorker odometry_;
    MotorBackend &motor_;
    ArmBackend &arm_;
    Metrics &metrics_;
    int log_every_;
    unsigned long long frame_count_ = 0;
    MotorOp last_op_ = MotorOp::Standby;
    int last_left_ = 1 << 20; // force the first drive command to emit
    int last_right_ = 1 << 20;
};

} // namespace tennis
