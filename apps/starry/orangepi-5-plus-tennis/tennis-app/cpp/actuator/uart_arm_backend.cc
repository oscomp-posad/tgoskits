// SPDX-License-Identifier: Apache-2.0
#include "uart_arm_backend.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>

namespace tennis {
namespace {

constexpr float kAngleMax = 270.0f;
constexpr float kOpen = 150.0f;
constexpr float kClosed = 90.0f;
constexpr int kStepDelayMs = 1000;
constexpr int kPositionTimeoutMs = 250;
constexpr int kGrabPulseThreshold = 1260;

void wait_for_step() {
    std::this_thread::sleep_for(std::chrono::milliseconds(kStepDelayMs));
}

} // namespace

UartArmBackend::UartArmBackend(const std::string &device) {
    ready_ = serial_.open(device, 115200, false);
}

bool UartArmBackend::send(const std::string &command) {
    if (serial_.write_all(command.data(), command.size()) && serial_.drain())
        return true;
    std::fprintf(stderr, "TENNIS_ERROR arm command failed: %s\n",
                 command.c_str());
    return false;
}

bool UartArmBackend::read_position(int servo, int &pulse) {
    char command[16];
    std::snprintf(command, sizeof(command), "#%03dPRAD!", servo);
    if (!send(command)) return false;

    std::array<char, 11> response{};
    size_t size = 0;
    while (size < response.size() - 1) {
        uint8_t byte = 0;
        if (!serial_.read_byte(byte, kPositionTimeoutMs)) {
            std::fprintf(stderr,
                         "TENNIS_ERROR arm position response timed out\n");
            return false;
        }
        if (size == 0 && byte != '#') continue;
        response[size++] = static_cast<char>(byte);
        if (byte == '!') break;
    }

    const auto digit = [&](size_t index) {
        return response[index] >= '0' && response[index] <= '9';
    };
    if (size != 10 || response[0] != '#' || response[4] != 'P' ||
        response[9] != '!' || !digit(1) || !digit(2) || !digit(3) ||
        !digit(5) || !digit(6) || !digit(7) || !digit(8)) {
        std::fprintf(stderr, "TENNIS_ERROR malformed arm position response\n");
        return false;
    }

    const int response_servo = (response[1] - '0') * 100 +
                               (response[2] - '0') * 10 +
                               (response[3] - '0');
    if (response_servo != servo) {
        std::fprintf(stderr,
                     "TENNIS_ERROR arm position response servo mismatch\n");
        return false;
    }
    pulse = (response[5] - '0') * 1000 + (response[6] - '0') * 100 +
            (response[7] - '0') * 10 + (response[8] - '0');
    return true;
}

bool UartArmBackend::set_angle(int servo, float angle, int time_ms) {
    angle = std::clamp(angle, 0.0f, kAngleMax);
    const int pulse = std::clamp(
        static_cast<int>(500.0f + angle / kAngleMax * 2000.0f), 500, 2500);
    char command[32];
    std::snprintf(command, sizeof(command), "#%03dP%04dT%d!", servo, pulse,
                  time_ms);
    return send(command);
}

bool UartArmBackend::set_pose(float servo0, float servo1, float servo2,
                              int time_ms) {
    const bool first = set_angle(0, servo0, time_ms);
    const bool second = set_angle(1, servo1, time_ms);
    return set_angle(2, servo2, time_ms) && first && second;
}

GrabResult UartArmBackend::grab() {
    if (!set_angle(2, kOpen)) return GrabResult::Error;
    wait_for_step();
    if (!set_pose(237.0f, 90.0f, kOpen)) return GrabResult::Error;
    wait_for_step();
    if (!set_angle(2, kClosed)) return GrabResult::Error;
    wait_for_step();

    int down_pulse = 0;
    const bool down_valid = read_position(2, down_pulse);

    // Always retract before deciding the result. The second sample verifies
    // that a ball detected at ground level remains held after lifting.
    if (!set_pose(130.0f, 30.0f, kClosed)) return GrabResult::Error;
    wait_for_step();
    int lifted_pulse = 0;
    const bool lifted_valid = read_position(2, lifted_pulse);
    const bool position_valid = down_valid && lifted_valid;
    const bool captured = position_valid &&
                          down_pulse >= kGrabPulseThreshold &&
                          lifted_pulse >= kGrabPulseThreshold;
    if (position_valid) {
        std::printf("TENNIS_ARM_GRAB down_pulse=%d lifted_pulse=%d "
                    "threshold=%d captured=%d\n",
                    down_pulse, lifted_pulse, kGrabPulseThreshold,
                    captured ? 1 : 0);
    }
    if (!position_valid) return GrabResult::Error;
    return captured ? GrabResult::Captured : GrabResult::Empty;
}

bool UartArmBackend::release() {
    if (!set_pose(160.0f, 110.0f, kClosed)) return false;
    wait_for_step();
    if (!set_angle(2, kOpen)) return false;
    wait_for_step();
    if (!set_pose(130.0f, 30.0f, kClosed)) return false;
    wait_for_step();
    return true;
}

bool UartArmBackend::ready() {
    if (!set_pose(160.0f, 30.0f, kClosed)) return false;
    wait_for_step();
    return true;
}

} // namespace tennis
