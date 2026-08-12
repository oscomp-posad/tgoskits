// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "actuator/motor_backend.h"
#include "odometry.h"
#include "types.h"

namespace tennis {

// Samples wheel telemetry and integrates odometry away from the perception and
// control loop. Readers only take the tracker lock long enough to copy a
// snapshot; a slow UART response never blocks a motor decision.
class OdometryWorker {
public:
    OdometryWorker(const Config &cfg, MotorBackend &motor);
    ~OdometryWorker();

    OdometryWorker(const OdometryWorker &) = delete;
    OdometryWorker &operator=(const OdometryWorker &) = delete;

    OdometryEstimate estimate(int64_t now_ns) const;
    void reset_anchor();

private:
    void run();

    MotorBackend &motor_;
    OdometryTracker tracker_;
    int sample_ms_;
    bool enabled_;

    mutable std::mutex tracker_mutex_;
    std::mutex wait_mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace tennis
