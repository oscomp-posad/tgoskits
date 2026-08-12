// SPDX-License-Identifier: Apache-2.0
//
// Thin sequenced-latest-frame wrapper over the reused uvc-rknn libuvc capture
// (uvc_capture.{h,cc}). libuvc runs its own capture thread and latches only the
// newest frame (drop-old), so a slow consumer never backs up capture. This
// wrapper adds: new-frame detection via the frame id (so we don't reprocess or
// needlessly copy an unchanged frame) and a capture timestamp for latency
// measurement. (No camera PTS is available from this libuvc build, so the stamp
// is taken at poll time -- see README "Next steps".)
//
// Board-only: requires libuvc at runtime; not part of the host dry-run build.
#pragma once

#include <cstdint>

#include "common.h"      // image_buffer_t (uvc-rknn utils)
#include "uvc_capture.h" // UvcCaptureSession, LatestFrame (uvc-rknn cpp)

namespace tennis {

class Camera {
public:
    ~Camera();

    bool start(int device, int width, int height, int fps);

    // Split start so live mode can overlap USB open/format negotiation with
    // RKNN model initialization, then defer the interrupt-heavy stream until
    // the model is ready.
    bool open_and_negotiate(int device, int width, int height, int fps);
    bool begin_streaming();

    void stop();

    // If a frame newer than the last polled one is available, copy it into
    // `frame`, stamp `capture_ts_ns`, and return true. Otherwise return false
    // (no copy performed -- only the frame id is peeked under the lock).
    bool poll(LatestFrame &frame, int64_t &capture_ts_ns);

    // Discard consecutive structurally valid frames until the stream is stable
    // enough for control, or fail after the bounded startup timeout.
    bool warm_up(int valid_frames, int timeout_ms);

    UvcCaptureCounters counters();

private:
    UvcCaptureSession session_;
    UvcCaptureOptions opts_{};
    uint64_t last_id_ = 0;
    bool started_ = false;
};

} // namespace tennis
