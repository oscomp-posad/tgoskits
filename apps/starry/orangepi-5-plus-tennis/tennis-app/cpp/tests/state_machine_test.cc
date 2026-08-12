// SPDX-License-Identifier: Apache-2.0
#include <cstdio>

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

tennis::Detection bucket_detection(float area) {
    tennis::Detection detection;
    detection.valid = true;
    detection.mode = tennis::PerceptionMode::BUCKET;
    detection.bucket.found = true;
    detection.bucket.area_ratio = area;
    detection.bucket.cx = 320.0f;
    return detection;
}

constexpr int64_t ms(int value) {
    return static_cast<int64_t>(value) * 1000000;
}

bool reverse_takes_priority_over_grab() {
    tennis::Config config;
    config.stop_confirm_cnt = 1;
    tennis::StateMachine machine(config);
    const auto output = machine.step(
        ball_detection(
            config.area_reverse + 0.1f,
            static_cast<float>(config.frame_w / 2 + config.stop_center_offset)),
        0);
    return expect(output.motor_op == tennis::MotorOp::Drive,
                  "an excessively close ball must command reverse") &&
           expect(output.left < 0 && output.right < 0,
                  "both wheels must reverse when the ball is too close") &&
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

bool ball_search_keeps_one_way_scan() {
    tennis::Config config;
    config.search_pivot_spd = 30;
    tennis::StateMachine machine(config);
    auto output = machine.step(tennis::Detection{}, ms(0));
    if (!expect(output.left == config.search_pivot_spd &&
                    output.right == -config.search_pivot_spd,
                "ball search must rotate in the initial direction"))
        return false;
    for (int frame = 1; frame <= 180; ++frame) {
        output = machine.step(tennis::Detection{}, ms(frame * 33));
        if (!expect(output.left == config.search_pivot_spd &&
                        output.right == -config.search_pivot_spd,
                    "ball search must keep rotating in one direction"))
            return false;
    }
    return true;
}

bool chase_uses_only_discrete_executable_actions() {
    tennis::Config config;
    tennis::StateMachine machine(config);
    const float target = static_cast<float>(
        config.frame_w / 2 + config.stop_center_offset);

    auto output = machine.step(ball_detection(0.1f, target - 50.0f), ms(0));
    if (!expect(output.motor_op == tennis::MotorOp::Drive &&
                    output.left == -config.chase_pivot_spd &&
                    output.right == config.chase_pivot_spd,
                "an offset ball must use a fixed executable pivot"))
        return false;
    output = machine.step(ball_detection(0.1f, target), ms(33));
    if (!expect(output.motor_op == tennis::MotorOp::Drive &&
                    output.left == config.chase_forward_spd &&
                    output.right == config.chase_forward_spd,
                "an aligned distant ball must use fixed forward speed"))
        return false;
    output = machine.step(
        ball_detection(config.area_stop + 0.01f, target), ms(66));
    if (!expect(output.motor_op == tennis::MotorOp::Brake,
                "an aligned close ball must brake"))
        return false;
    output = machine.step(
        ball_detection(config.area_stop + 0.01f, target + 50.0f), ms(99));
    return expect(output.motor_op == tennis::MotorOp::Drive &&
                      output.left == config.chase_pivot_spd &&
                      output.right == -config.chase_pivot_spd,
                  "a close offset ball must pivot without a timed sub-state");
}

bool captured_ball_uses_odom_return_and_visual_takeover() {
    tennis::Config config;
    config.stop_confirm_cnt = 1;
    config.bucket_confirm_cnt = 1;
    config.brake_hold_ms = 0;
    config.grab_settle_ms = 0;
    config.release_settle_ms = 0;
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
    if (!expect(output.arm == tennis::ArmAction::Ready &&
                    machine.state() == tennis::GameState::CHASE_BALL,
                "deposit completion must return to ball chase"))
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

bool deposit_waits_for_brake_and_release() {
    tennis::Config config;
    config.stop_confirm_cnt = 1;
    config.bucket_confirm_cnt = 1;
    config.brake_hold_ms = 350;
    config.grab_settle_ms = 0;
    config.release_settle_ms = 500;
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
    return expect(output.arm == tennis::ArmAction::Ready &&
                      machine.state() == tennis::GameState::CHASE_BALL,
                  "arm must return home after the release deadline");
}

} // namespace

int main() {
    if (!reverse_takes_priority_over_grab()) return 1;
    if (!grab_brake_uses_elapsed_time()) return 1;
    if (!empty_grab_resumes_ball_search()) return 1;
    if (!ball_search_keeps_one_way_scan()) return 1;
    if (!chase_uses_only_discrete_executable_actions()) return 1;
    if (!captured_ball_uses_odom_return_and_visual_takeover()) return 1;
    if (!deposit_waits_for_brake_and_release()) return 1;
    std::puts("tennis_state_machine_test: OK");
    return 0;
}
