// SPDX-License-Identifier: Apache-2.0
#include <cstdio>

#include "actuator/motor_dead_zone.h"
#include "state_machine.h"

namespace {

bool expect(bool condition, const char *message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

tennis::Detection ball_detection(float area, float cx) {
    tennis::Detection detection;
    detection.valid = true;
    detection.mode = tennis::PerceptionMode::BALL;
    detection.ball.found = true;
    detection.ball.area_ratio = area;
    detection.ball.cx = cx;
    return detection;
}

tennis::Detection bucket_detection(float area, float cx = 320.0f) {
    tennis::Detection detection;
    detection.valid = true;
    detection.mode = tennis::PerceptionMode::BUCKET;
    detection.bucket.found = true;
    detection.bucket.area_ratio = area;
    detection.bucket.cx = cx;
    return detection;
}

constexpr int64_t ms(int value) {
    return static_cast<int64_t>(value) * 1000000;
}

bool reverse_takes_priority_over_grab() {
    tennis::Config config;
    config.stop_confirm_cnt = 1;
    config.motor_min_speed = 28;
    config.reverse_speed = 30;
    tennis::StateMachine machine(config);
    const auto output = machine.step(
        ball_detection(
            config.area_reverse,
            static_cast<float>(config.frame_w / 2 + config.stop_center_offset)),
        0);
    return expect(output.motor_op == tennis::MotorOp::Drive &&
                      output.left == -30 && output.right == -30,
                  "the reverse boundary must use the configured speed") &&
           expect(machine.state() == tennis::GameState::CHASE_BALL,
                  "reverse must not transition to grab");
}

bool grab_brake_uses_elapsed_time() {
    tennis::Config config;
    config.stop_confirm_cnt = 1;
    config.brake_hold_ms = 350;
    config.grab_settle_ms = 100;
    tennis::StateMachine machine(config);
    const auto detection = ball_detection(
        config.area_stop + 0.01f,
        static_cast<float>(config.frame_w / 2 + config.stop_center_offset));

    auto output = machine.step(detection, ms(0));
    if (!expect(output.motor_op == tennis::MotorOp::Brake,
                "grab approach must begin active braking"))
        return false;
    output = *machine.tick(ms(349));
    if (!expect(output.motor_op == tennis::MotorOp::Brake,
                "grab brake must remain active until its deadline"))
        return false;
    output = *machine.tick(ms(350));
    if (!expect(output.motor_op == tennis::MotorOp::Standby &&
                    output.arm == tennis::ArmAction::Grab,
                "grab must start only after braking completes"))
        return false;
    if (!expect(machine.state() == tennis::GameState::GRAB,
                "grab action must be represented by the GRAB state"))
        return false;
    machine.tick(ms(449));
    if (!expect(machine.state() == tennis::GameState::GRAB,
                "grab settle must use elapsed time, not frame count"))
        return false;
    machine.tick(ms(450));
    return expect(machine.state() == tennis::GameState::FIND_BUCKET,
                  "grab must finish at its elapsed-time deadline");
}

bool empty_grab_resumes_ball_search() {
    tennis::Config config;
    config.stop_confirm_cnt = 1;
    config.brake_hold_ms = 0;
    tennis::StateMachine machine(config);
    const auto detection = ball_detection(
        config.area_stop + 0.01f,
        static_cast<float>(config.frame_w / 2 + config.stop_center_offset));

    machine.step(detection, ms(0));
    const auto output = machine.tick(ms(0));
    if (!expect(output && output->arm == tennis::ArmAction::Grab &&
                    machine.state() == tennis::GameState::GRAB,
                "test setup must enter the grab state"))
        return false;
    machine.on_grab_empty();
    return expect(machine.state() == tennis::GameState::CHASE_BALL,
                  "an empty grasp must resume ball search") &&
           expect(machine.perception_mode() == tennis::PerceptionMode::BALL,
                  "an empty grasp must restore ball perception");
}

bool ball_search_reverses_after_estimated_two_turns() {
    tennis::Config config;
    config.search_pivot_spd = 30;
    config.search_reverse_turns = 2.0;
    tennis::StateMachine machine(config);
    auto output = machine.step(tennis::Detection{}, ms(0));
    if (!expect(output.left == config.search_pivot_spd &&
                    output.right == -config.search_pivot_spd,
                "ball search must rotate in the initial direction"))
        return false;
    output = machine.step(tennis::Detection{}, ms(11999));
    if (!expect(output.left == config.search_pivot_spd &&
                    output.right == -config.search_pivot_spd,
                "ball search must keep direction before two estimated turns"))
        return false;
    output = machine.step(tennis::Detection{}, ms(12000));
    return expect(output.left == -config.search_pivot_spd &&
                      output.right == config.search_pivot_spd,
                  "ball search must reverse after two estimated turns");
}

bool chase_uses_proportional_executable_steering() {
    tennis::Config config;
    config.motor_min_speed = 30;
    config.chase_far_spd = 45;
    config.chase_forward_spd = 30;
    config.chase_turn_k = 24.0f;
    config.chase_max_bias = 24;
    config.chase_close_pivot_spd = 30;
    tennis::StateMachine machine(config);
    const float target = static_cast<float>(
        config.frame_w / 2 + config.stop_center_offset);

    auto output = machine.step(ball_detection(0.1f, target - 10.0f), ms(0));
    if (!expect(output.motor_op == tennis::MotorOp::Drive &&
                    output.left == config.chase_far_spd &&
                    output.right == config.chase_far_spd,
                "a ball inside the horizontal dead zone must drive straight"))
        return false;

    output = machine.step(ball_detection(0.1f, target + 100.0f), ms(33));
    if (!expect(output.motor_op == tennis::MotorOp::Drive &&
                    output.left == 53 && output.right == 37,
                "far pursuit must add proportional symmetric steering bias"))
        return false;

    output = machine.step(
        ball_detection(config.area_far, target + 50.0f), ms(50));
    if (!expect(output.motor_op == tennis::MotorOp::Drive &&
                    output.left == 38 && output.right == 30,
                "near pursuit must keep the inside wheel at the physical floor"))
        return false;

    output = machine.step(
        ball_detection(config.area_far, target - 50.0f), ms(66));
    if (!expect(output.motor_op == tennis::MotorOp::Drive &&
                    output.left == 30 && output.right == 38,
                "left steering must mirror right steering"))
        return false;

    output = machine.step(
        ball_detection(config.area_far, target - 320.0f), ms(83));
    if (!expect(output.motor_op == tennis::MotorOp::Drive &&
                    output.left == 30 && output.right == 78,
                "steering bias must saturate at the configured maximum"))
        return false;

    output = machine.step(
        ball_detection(config.area_stop + 0.01f, target), ms(99));
    if (!expect(output.motor_op == tennis::MotorOp::Brake,
                "an aligned close ball must brake"))
        return false;
    output = machine.step(
        ball_detection(config.area_stop + 0.01f, target + 50.0f), ms(116));
    return expect(output.motor_op == tennis::MotorOp::Drive &&
                      output.left == 30 && output.right == -30,
                  "a close but offset ball must pivot without moving forward");
}

bool motor_dead_zone_maps_each_wheel_independently() {
    auto command = tennis::apply_motor_dead_zone(25, 25, 30);
    if (!expect(command.left == 30 && command.right == 30,
                "each low straight wheel must reach the speed floor"))
        return false;

    command = tennis::apply_motor_dead_zone(33, 17, 30);
    if (!expect(command.left == 33 && command.right == 30,
                "only the wheel below the floor may be lifted"))
        return false;

    command = tennis::apply_motor_dead_zone(13, -3, 30);
    if (!expect(command.left == 30 && command.right == -30,
                "opposite low wheel speeds must preserve their signs"))
        return false;

    command = tennis::apply_motor_dead_zone(38, -30, 30);
    if (!expect(command.left == 38 && command.right == -30,
                "executable asymmetric counter-rotation must stay unchanged"))
        return false;

    command = tennis::apply_motor_dead_zone(12, -12, 30);
    return expect(command.left == 30 && command.right == -30,
                  "a true in-place turn must lift both wheel magnitudes");
}

bool bucket_approach_uses_configured_speed_bands() {
    tennis::Config config;
    config.stop_confirm_cnt = 1;
    config.bucket_confirm_cnt = 1;
    config.brake_hold_ms = 0;
    config.grab_settle_ms = 0;
    config.motor_min_speed = 30;
    config.bucket_approach_spd = 45;
    config.bucket_brake_spd = 35;
    tennis::StateMachine machine(config);
    const float target = static_cast<float>(
        config.frame_w / 2 + config.stop_center_offset);

    machine.step(ball_detection(config.area_stop + 0.01f, target), ms(0));
    machine.tick(ms(0));
    machine.tick(ms(0));
    machine.step(bucket_detection(0.1f), ms(0));

    auto output = machine.step(
        bucket_detection(config.bucket_area_brake, 448.0f), ms(33));
    if (!expect(output.motor_op == tennis::MotorOp::Drive &&
                    output.left == 43 && output.right == 27,
                "near bucket pursuit must steer around base speed 35"))
        return false;

    output = machine.step(
        bucket_detection(config.bucket_area_brake, 192.0f), ms(66));
    if (!expect(output.motor_op == tennis::MotorOp::Drive &&
                    output.left == 27 && output.right == 43,
                "near bucket steering must be mirrored"))
        return false;

    output = machine.step(bucket_detection(0.1f, 448.0f), ms(99));
    return expect(output.motor_op == tennis::MotorOp::Drive &&
                      output.left == 53 && output.right == 37,
                  "far bucket pursuit must steer around base speed 45");
}

bool bucket_search_uses_configured_in_place_speed() {
    tennis::Config config;
    config.stop_confirm_cnt = 1;
    config.brake_hold_ms = 0;
    config.grab_settle_ms = 0;
    config.bucket_search_spd = 35;
    tennis::StateMachine machine(config);
    const float target = static_cast<float>(
        config.frame_w / 2 + config.stop_center_offset);

    machine.step(ball_detection(config.area_stop + 0.01f, target), ms(0));
    machine.tick(ms(0));
    machine.tick(ms(0));
    const auto output = machine.step(tennis::Detection{}, ms(33));
    return expect(output.motor_op == tennis::MotorOp::Drive &&
                      output.left == 35 && output.right == -35,
                  "bucket search must use the configured in-place speed");
}

bool bucket_search_reverses_after_estimated_two_turns() {
    tennis::Config config;
    config.stop_confirm_cnt = 1;
    config.brake_hold_ms = 0;
    config.grab_settle_ms = 0;
    config.bucket_search_spd = 30;
    config.search_reverse_turns = 2.0;
    tennis::StateMachine machine(config);
    const float target = static_cast<float>(
        config.frame_w / 2 + config.stop_center_offset);

    machine.step(ball_detection(config.area_stop + 0.01f, target), ms(0));
    machine.tick(ms(0));
    machine.tick(ms(0));
    auto output = machine.step(tennis::Detection{}, ms(0));
    if (!expect(output.left == config.bucket_search_spd &&
                    output.right == -config.bucket_search_spd,
                "bucket search must rotate in the initial direction"))
        return false;
    output = machine.step(tennis::Detection{}, ms(12000));
    return expect(output.left == -config.bucket_search_spd &&
                      output.right == config.bucket_search_spd,
                  "bucket search must reverse after two estimated turns");
}

bool captured_ball_uses_odom_return_and_visual_takeover() {
    tennis::Config config;
    config.stop_confirm_cnt = 1;
    config.bucket_confirm_cnt = 1;
    config.brake_hold_ms = 0;
    config.grab_settle_ms = 0;
    config.release_settle_ms = 0;
    config.deposit_reverse_ms = 0;
    tennis::StateMachine machine(config);
    const float target = static_cast<float>(
        config.frame_w / 2 + config.stop_center_offset);
    const auto ball = ball_detection(config.area_stop + 0.01f, target);

    machine.step(ball, ms(0));
    auto output = *machine.tick(ms(0));
    if (!expect(output.arm == tennis::ArmAction::Grab &&
                    machine.state() == tennis::GameState::GRAB,
                "first cycle must reach grab"))
        return false;
    machine.tick(ms(0));
    if (!expect(machine.state() == tennis::GameState::FIND_BUCKET,
                "without a bucket anchor the first cycle uses visual search"))
        return false;
    machine.step(bucket_detection(0.1f), ms(0));
    machine.step(bucket_detection(1.0f), ms(0));
    machine.tick(ms(0));
    output = *machine.tick(ms(0));
    if (!expect(output.arm == tennis::ArmAction::None &&
                    output.reset_odometry &&
                    machine.state() == tennis::GameState::CHASE_BALL,
                "deposit completion must reset odometry and chase the ball"))
        return false;

    tennis::OdometryEstimate estimate;
    estimate.anchor_set = true;
    estimate.valid = true;
    estimate.x = 1.0;
    estimate.distance_to_anchor = 1.0;
    estimate.bearing_to_anchor = 3.14159265358979323846;
    machine.set_odometry(estimate);
    machine.step(ball, ms(0));
    machine.tick(ms(0));
    machine.tick(ms(0));
    if (!expect(machine.state() == tennis::GameState::RETURN_TO_BUCKET &&
                    machine.perception_mode() == tennis::PerceptionMode::BUCKET,
                "a later captured ball must enter odometry return"))
        return false;

    output = machine.step(tennis::Detection{}, ms(1));
    if (!expect(output.motor_op == tennis::MotorOp::Drive &&
                    output.left == -config.return_pivot_spd &&
                    output.right == config.return_pivot_spd,
                "return must pivot toward the bucket anchor"))
        return false;
    output = machine.step(bucket_detection(0.1f), ms(2));
    return expect(output.motor_op == tennis::MotorOp::Brake &&
                      machine.state() == tennis::GameState::FIND_BUCKET,
                  "visual bucket detection must immediately take over");
}

bool deposit_waits_for_brake_release_and_reverse() {
    tennis::Config config;
    config.stop_confirm_cnt = 1;
    config.bucket_confirm_cnt = 1;
    config.brake_hold_ms = 350;
    config.grab_settle_ms = 0;
    config.release_settle_ms = 500;
    config.deposit_reverse_speed = 30;
    config.deposit_reverse_ms = 500;
    tennis::StateMachine machine(config);
    const auto ball = ball_detection(
        config.area_stop + 0.01f,
        static_cast<float>(config.frame_w / 2 + config.stop_center_offset));

    machine.step(ball, ms(0));
    machine.tick(ms(350));
    machine.tick(ms(350));
    if (!expect(machine.state() == tennis::GameState::FIND_BUCKET,
                "test setup must reach bucket search"))
        return false;

    machine.step(bucket_detection(0.1f), ms(350));
    auto output = machine.step(bucket_detection(1.0f), ms(350));
    if (!expect(output.motor_op == tennis::MotorOp::Brake,
                "deposit approach must begin active braking"))
        return false;
    output = *machine.tick(ms(699));
    if (!expect(output.motor_op == tennis::MotorOp::Brake,
                "deposit brake must remain active until its deadline"))
        return false;
    output = *machine.tick(ms(700));
    if (!expect(output.arm == tennis::ArmAction::Release,
                "release must start after deposit braking"))
        return false;
    machine.tick(ms(1199));
    if (!expect(machine.state() == tennis::GameState::DEPOSIT,
                "deposit must wait for the release deadline"))
        return false;
    output = *machine.tick(ms(1200));
    if (!expect(output.arm == tennis::ArmAction::None &&
                    output.motor_op == tennis::MotorOp::Drive &&
                    output.left == -30 && output.right == -30 &&
                    !output.reset_odometry &&
                    machine.state() == tennis::GameState::DEPOSIT &&
                    machine.perception_mode() == tennis::PerceptionMode::BUCKET,
                "release completion must reverse without a redundant ready pose"))
        return false;
    output = *machine.tick(ms(1699));
    if (!expect(output.motor_op == tennis::MotorOp::Drive &&
                    output.left == -30 && output.right == -30 &&
                    machine.state() == tennis::GameState::DEPOSIT,
                "deposit reverse must continue until its elapsed-time deadline"))
        return false;
    output = *machine.tick(ms(1700));
    return expect(output.motor_op == tennis::MotorOp::Standby &&
                      output.reset_odometry &&
                      machine.state() == tennis::GameState::CHASE_BALL &&
                      machine.perception_mode() == tennis::PerceptionMode::BALL,
                  "reverse completion must stop, reset odometry, then search");
}

bool single_dropout_coasts_not_spin() {
    tennis::Config config;
    tennis::StateMachine machine(config);
    // Ball far, right of centre -> forward drive with a steering bias.
    const auto out1 = machine.step(
        ball_detection(0.10f, static_cast<float>(config.frame_w / 2 + 30)),
        ms(0));
    if (!expect(out1.motor_op == tennis::MotorOp::Drive && out1.left > 0 &&
                    out1.right > 0,
                "a found ball must drive forward (both wheels positive)"))
        return false;
    // One dropped frame must coast the exact last command, not flip to a spin.
    const auto out2 = machine.step(tennis::Detection{}, ms(33));
    return expect(out2.motor_op == out1.motor_op && out2.left == out1.left &&
                      out2.right == out1.right,
                  "a single dropped frame must coast the last pursuit command");
}

bool sustained_loss_falls_to_scan() {
    tennis::Config config;
    config.chase_lost_coast_frames = 2;
    config.search_pivot_spd = 35;
    tennis::StateMachine machine(config);
    machine.step(
        ball_detection(0.10f, static_cast<float>(config.frame_w / 2 + 30)),
        ms(0));                                     // seen once (offset > 0)
    machine.step(tennis::Detection{}, ms(33));      // coast 1
    machine.step(tennis::Detection{}, ms(66));      // coast 2
    const auto out = machine.step(tennis::Detection{}, ms(99)); // budget spent
    return expect(out.motor_op == tennis::MotorOp::Drive &&
                      out.left == config.search_pivot_spd &&
                      out.right == -config.search_pivot_spd,
                  "after the coast budget a sustained loss falls to one-way scan");
}

bool fresh_chase_never_coasts_stale() {
    tennis::Config config;
    config.search_pivot_spd = 35;
    tennis::StateMachine machine(config);
    // No ball seen this episode -> a lost frame scans immediately, no coast.
    const auto out = machine.step(tennis::Detection{}, ms(0));
    return expect(out.motor_op == tennis::MotorOp::Drive &&
                      out.left == config.search_pivot_spd &&
                      out.right == -config.search_pivot_spd,
                  "with no prior sighting a lost frame scans at once");
}

} // namespace

int main() {
    if (!reverse_takes_priority_over_grab()) return 1;
    if (!grab_brake_uses_elapsed_time()) return 1;
    if (!empty_grab_resumes_ball_search()) return 1;
    if (!ball_search_reverses_after_estimated_two_turns()) return 1;
    if (!chase_uses_proportional_executable_steering()) return 1;
    if (!motor_dead_zone_maps_each_wheel_independently()) return 1;
    if (!bucket_approach_uses_configured_speed_bands()) return 1;
    if (!bucket_search_uses_configured_in_place_speed()) return 1;
    if (!bucket_search_reverses_after_estimated_two_turns()) return 1;
    if (!captured_ball_uses_odom_return_and_visual_takeover()) return 1;
    if (!deposit_waits_for_brake_release_and_reverse()) return 1;
    if (!single_dropout_coasts_not_spin()) return 1;
    if (!sustained_loss_falls_to_scan()) return 1;
    if (!fresh_chase_never_coasts_stale()) return 1;
    std::puts("tennis_state_machine_test: OK");
    return 0;
}
