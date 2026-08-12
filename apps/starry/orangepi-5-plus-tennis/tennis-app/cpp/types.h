// SPDX-License-Identifier: Apache-2.0
//
// Core value types and tunable configuration for the StarryOS tennis-ball
// pickup demo. All thresholds/gains live in `Config` (fields, not #defines) so
// the chassis-specific tuning can be overridden from the command line without
// recompiling. The numeric defaults are a clean re-derivation of the reference
// aka-rk3588 behaviour (the reference repo carries no license; see README).
#pragma once

#include <cstdint>

namespace tennis {

// The five game states required by the demo. GRAB and DEPOSIT are modelled as
// explicit (brief, non-blocking) states rather than folded into a transition,
// so the state trace is unambiguous.
enum class GameState {
    CHASE_BALL,
    GRAB,
    FIND_BUCKET,
    APPROACH_BUCKET,
    DEPOSIT,
};

const char *to_string(GameState s);

// Which detector the perception stage should run for the next frame. The
// control thread publishes this based on the current GameState so the
// perception thread never does redundant work (the reference decoded the same
// JPEG twice in bucket states).
enum class PerceptionMode {
    BALL,   // YOLOv8 tennis-ball detection
    BUCKET, // HSV red-bucket detection
};

const char *to_string(PerceptionMode m);

// A single tennis-ball observation in original camera-frame pixel coordinates.
struct BallObs {
    bool found = false;
    float cx = 0.f; // box centre x
    float cy = 0.f; // box centre y
    float w = 0.f;
    float h = 0.f;
    float area_ratio = 0.f; // (w*h) / (frame_w*frame_h)
    float score = 0.f;
};

// A single red-bucket observation in original camera-frame pixel coordinates.
struct BucketObs {
    bool found = false;
    float cx = 0.f;
    float cy = 0.f;
    float area_ratio = 0.f; // largest-blob pixel count / frame area
    int x = 0, y = 0, w = 0, h = 0;
};

// One published perception result. `capture_ts_ns` is stamped when the camera
// frame was latched; `detect_ts_ns` when perception finished. Both use a
// monotonic clock so latency deltas are meaningful.
struct Detection {
    uint64_t seq = 0;
    int64_t capture_ts_ns = 0;
    int64_t detect_ts_ns = 0;
    PerceptionMode mode = PerceptionMode::BALL;
    bool valid = false;
    BallObs ball;
    BucketObs bucket;
};

// All tunable parameters. Defaults re-derived from the reference robot; every
// value is overridable via CLI so a different chassis/camera/model can be tuned
// without a rebuild.
struct Config {
    // Frame / model geometry.
    int frame_w = 640;
    int frame_h = 480;
    int model_w = 640; // overwritten at runtime from the .rknn input attrs
    int model_h = 640;

    // Detector.
    int ball_class_id = 0;     // 0 for a single-class tennis model; 32 (sports ball) for COCO yolov8.rknn
    float conf_thresh = 0.5f;
    float nms_thresh = 0.45f;

    // CHASE_BALL distance zones (by ball area_ratio).
    float area_far = 0.02f;
    float area_near = 0.35f;
    float area_brake = 0.20f;
    float area_stop = 0.28f;
    float area_reverse = 0.50f;
    float area_stop_exit = 0.20f;

    // CHASE_BALL speeds / steering.
    int chase_speed_far = 40;
    int brake_speed = 3;
    int reverse_speed = 15;
    float k_turn = 25.0f;
    int max_turn_bias_far = 5;
    int max_turn_bias_near = 10;
    int center_dead_zone = 15;

    // Stop / grab gate. The ball is centred under a gripper mounted right of the
    // optical centre, hence the off-centre stop target.
    int stop_center_offset = 90;
    int stop_center_zone = 5;
    int stop_confirm_cnt = 4;

    // ALIGN (proportional pivot to centre the ball) + stall kick.
    int align_pivot_spd = 15;
    int align_pivot_min = 3;
    int align_stall_frames = 20;
    int align_stall_move_px = 10;
    int align_kick_spd = 35;

    // Ball lost search.
    int search_frames = 25;
    int search_pivot_spd = 10;
    int scan_flip_frames = 60;

    // Bucket approach.
    float bucket_area_deposit = 0.90f;
    float bucket_area_brake = 0.70f;
    int bucket_approach_spd = 25;
    int bucket_brake_spd = 5;
    float bucket_k_turn = 20.0f;
    int bucket_max_bias = 8;
    int bucket_search_spd = 12;
    int bucket_lost_frames = 10;
    int bucket_confirm_cnt = 3;

    // HSV red mask (H in [0,360), S/V in [0,255]).
    int hsv_s_min = 80;
    int hsv_v_min = 50;
    int hsv_h_lo = 20;   // accept H <= hsv_h_lo
    int hsv_h_hi = 340;  // or H >= hsv_h_hi
    int bucket_min_area = 1000;

    // Motor dead-zone: a non-zero command is lifted to at least min_speed so it
    // overcomes stall. Lives in the abstract Motor layer, not the backend.
    int motor_min_speed = 15;

    // Brief settle windows (frames) for the otherwise-instant virtual arm moves,
    // so GRAB/DEPOSIT are observable states rather than zero-width transitions.
    int grab_settle_frames = 3;
    int deposit_settle_frames = 3;

    // Drop a detection whose source frame is older than this (0 = never). Keeps
    // control acting on fresh perception under load.
    int staleness_ms = 0;
};

} // namespace tennis
