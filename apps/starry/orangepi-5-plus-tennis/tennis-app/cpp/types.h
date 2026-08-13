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

// GRAB and DEPOSIT are modelled as
// explicit (brief, non-blocking) states rather than folded into a transition,
// so the state trace is unambiguous.
enum class GameState {
    CHASE_BALL,
    GRAB,
    RETURN_TO_BUCKET,
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
    float conf_thresh = 0.75f;
    float nms_thresh = 0.45f;

    // CHASE_BALL distance limits (by ball area_ratio).
    float area_far = 0.20f;
    float area_stop = 0.56f;
    float area_reverse = 0.62f;

    // Ball pursuit chassis commands. Steering bias is proportional to the
    // horizontal error and is composed here above the physical speed floor.
    int chase_far_spd = 45;
    int chase_forward_spd = 30;
    float chase_turn_k = 24.0f;
    int chase_max_bias = 24;
    int chase_close_pivot_spd = 30;
    int reverse_speed = 30;
    int center_dead_zone = 15;

    // Stop / grab gate. The ball is centred under a gripper mounted right of the
    // optical centre, hence the off-centre stop target.
    int stop_center_offset = 108;
    int stop_center_zone = 20;
    int stop_confirm_cnt = 3;

    // Ball lost search.
    int search_pivot_spd = 35;
    double search_reverse_turns = 2.0;

    // Optional wheel-RPM odometry. The generic default remains off; the
    // calibrated Orange Pi live configuration enables it explicitly.
    bool odometry_enabled = false;
    double odometry_wheel_radius_m = 0.03;
    double odometry_wheel_base_m = 0.18;
    int odometry_sample_ms = 100;
    int odometry_stale_ms = 500;
    int odometry_max_gap_ms = 500;
    int odometry_max_rpm = 300;

    // Coarse return-to-bucket guidance; vision takes priority in this state.
    double return_heading_tolerance_deg = 15.0;
    double return_stop_radius_m = 0.50;
    double return_max_distance_m = 10.0;
    int return_timeout_ms = 15000;
    int return_pivot_spd = 30;
    int return_forward_spd = 30;

    // Bucket approach.
    float bucket_area_deposit = 0.90f;
    float bucket_area_brake = 0.70f;
    int bucket_approach_spd = 45;
    int bucket_brake_spd = 35;
    float bucket_k_turn = 20.0f;
    int bucket_max_bias = 8;
    int bucket_search_spd = 35;
    int bucket_lost_frames = 10;
    int bucket_confirm_cnt = 3;

    // HSV red mask (H in [0,360), S/V in [0,255]).
    int hsv_s_min = 80;
    int hsv_v_min = 50;
    int hsv_h_lo = 20;   // accept H <= hsv_h_lo
    int hsv_h_hi = 340;  // or H >= hsv_h_hi
    int bucket_min_area = 3000;

    // Motor dead-zone: every non-zero wheel command is independently lifted to
    // this physical floor. Base-speed/bias composition stays in the FSM.
    int motor_min_speed = 28;

    // Physical action timing. Timed motor phases remain non-blocking, while the
    // state machine keeps issuing the latched command until its deadline.
    int brake_hold_ms = 200;
    int grab_settle_ms = 100;
    int release_settle_ms = 500;
    int deposit_reverse_speed = 30;
    int deposit_reverse_ms = 500;

    // Drop a detection whose source frame is older than this (0 = never). Keeps
    // control acting on fresh perception under load.
    int staleness_ms = 0;
};

} // namespace tennis
