// SPDX-License-Identifier: Apache-2.0
#include "camera.h"

#include <mutex>

#include "time_utils.h"

namespace tennis {

Camera::~Camera() { stop(); }

bool Camera::start(int device, int width, int height, int fps) {
    return open_and_negotiate(device, width, height, fps) &&
           begin_streaming();
}

bool Camera::open_and_negotiate(int device, int width, int height, int fps) {
    opts_ = UvcCaptureOptions{};
    opts_.device = device;
    opts_.width = width;
    opts_.height = height;
    opts_.fps = fps;
    opts_.log_prefix = "tennis-uvc";
    // A successful open owns resources even before streaming begins, so stop()
    // must close them if model initialization later fails.
    started_ = uvc_open_and_negotiate(&session_, &opts_);
    return started_;
}

bool Camera::begin_streaming() {
    if (!started_) return false;
    if (uvc_begin_streaming(&session_, &opts_)) return true;
    // uvc_begin_streaming cleans up the session on failure.
    started_ = false;
    return false;
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

bool Camera::warm_up(int valid_frames, int timeout_ms) {
    if (valid_frames <= 0) return true;
    const int64_t deadline = monotonic_ns() +
                             static_cast<int64_t>(timeout_ms) * 1000000;
    int consecutive = 0;
    while (monotonic_ns() < deadline) {
        LatestFrame frame;
        int64_t capture_ts_ns = 0;
        if (!poll(frame, capture_ts_ns)) {
            sleep_ns(1000000);
            continue;
        }
        if (frame.width > 0 && frame.height > 0 && !frame.data.empty()) {
            if (++consecutive >= valid_frames) return true;
        } else {
            consecutive = 0;
        }
    }
    return false;
}

UvcCaptureCounters Camera::counters() {
    return capture_counters(&session_.state);
}

} // namespace tennis
