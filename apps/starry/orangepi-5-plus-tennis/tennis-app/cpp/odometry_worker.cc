// SPDX-License-Identifier: Apache-2.0
#include "odometry_worker.h"

#include <chrono>

#include "time_utils.h"

namespace tennis {

OdometryWorker::OdometryWorker(const Config &cfg, MotorBackend &motor)
    : motor_(motor), tracker_(cfg), sample_ms_(cfg.odometry_sample_ms),
      enabled_(cfg.odometry_enabled) {
    if (enabled_) thread_ = std::thread(&OdometryWorker::run, this);
}

OdometryWorker::~OdometryWorker() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(wait_mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    thread_.join();
}

OdometryEstimate OdometryWorker::estimate(int64_t now_ns) const {
    std::lock_guard<std::mutex> lock(tracker_mutex_);
    return tracker_.estimate(now_ns);
}

void OdometryWorker::reset_anchor() {
    std::lock_guard<std::mutex> lock(tracker_mutex_);
    tracker_.reset_anchor();
}

void OdometryWorker::run() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(wait_mutex_);
            if (stopping_) return;
        }

        WheelRpm rpm;
        if (motor_.read_wheel_rpm(rpm) == TelemetryResult::Sample) {
            const int64_t sample_ns = monotonic_ns();
            std::lock_guard<std::mutex> lock(tracker_mutex_);
            (void)tracker_.update(rpm.left, rpm.right, sample_ns);
        }

        std::unique_lock<std::mutex> lock(wait_mutex_);
        if (wake_.wait_for(lock, std::chrono::milliseconds(sample_ms_),
                           [this] { return stopping_; }))
            return;
    }
}

} // namespace tennis
