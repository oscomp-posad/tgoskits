// SPDX-License-Identifier: Apache-2.0
#include "camera.h"

#include <mutex>

#include "time_utils.h"

namespace tennis {

Camera::~Camera() { stop(); }

bool Camera::start(int device, int width, int height, int fps) {
    UvcCaptureOptions opts;
    opts.device = device;
    opts.width = width;
    opts.height = height;
    opts.fps = fps;
    opts.log_prefix = "tennis-uvc";
    started_ = start_uvc_capture(&session_, &opts);
    return started_;
}

void Camera::stop() {
    if (started_) {
        stop_uvc_capture(&session_);
        started_ = false;
    }
}

bool Camera::poll(LatestFrame &frame, int64_t &capture_ts_ns) {
    // Cheap peek: only the id under the lock; skip the full copy if unchanged.
    {
        std::lock_guard<std::mutex> guard(session_.state.mutex);
        if (session_.state.latest.id == last_id_) return false;
    }
    if (!snapshot_latest_capture(&session_.state, &frame)) return false;
    if (frame.id == last_id_) return false;
    last_id_ = frame.id;
    capture_ts_ns = monotonic_ns();
    return true;
}

UvcCaptureCounters Camera::counters() {
    return capture_counters(&session_.state);
}

} // namespace tennis
