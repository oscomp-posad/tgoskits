// SPDX-License-Identifier: Apache-2.0
#include "uart_motor_backend.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace tennis {
namespace {

constexpr uint8_t kSof0 = 0xaa;
constexpr uint8_t kSof1 = 0x55;
constexpr uint8_t kCmdInit = 0x01;
constexpr uint8_t kCmdConfig = 0x02;
constexpr uint8_t kCmdSetSpeeds = 0x13;
constexpr uint8_t kCmdStop = 0x11;
constexpr uint8_t kCmdGetRpm = 0x20;
constexpr uint8_t kRspAck = 0x80;
constexpr uint8_t kRspRpmData = 0x90;

uint8_t checksum(uint8_t command, uint8_t size, const uint8_t *payload) {
    uint8_t value = command ^ size;
    for (uint8_t i = 0; i < size; ++i) value ^= payload[i];
    return value;
}

void put_be16(uint8_t *buffer, uint16_t value) {
    buffer[0] = static_cast<uint8_t>(value >> 8);
    buffer[1] = static_cast<uint8_t>(value);
}

int16_t get_be16(const uint8_t *buffer) {
    return static_cast<int16_t>((static_cast<uint16_t>(buffer[0]) << 8) |
                                static_cast<uint16_t>(buffer[1]));
}

} // namespace

UartMotorBackend::UartMotorBackend(const std::string &device, uint16_t ppr,
                                   uint16_t pwm_frequency) {
    if (!serial_.open(device)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    serial_.flush();
    if (!send_command(kCmdInit, nullptr, 0)) return;
    uint8_t config[4];
    put_be16(config, ppr);
    put_be16(config + 2, pwm_frequency);
    if (!send_command(kCmdConfig, config, sizeof(config))) return;
    ready_ = true;
}

UartMotorBackend::~UartMotorBackend() {
    if (serial_.is_open()) (void)stop();
}

bool UartMotorBackend::send_command(uint8_t command, const uint8_t *payload,
                                    uint8_t size, bool expect_ack) {
    if (size > 32) return false;
    uint8_t frame[37] = {kSof0, kSof1, command, size};
    if (size > 0) std::memcpy(frame + 4, payload, size);
    frame[4 + size] = checksum(command, size, payload);
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (!serial_.write_all(frame, 5 + size)) return false;
    }
    if (!expect_ack || receive_ack(500)) return true;
    std::fprintf(stderr, "TENNIS_ERROR motor command 0x%02x failed\n",
                 command);
    return false;
}

bool UartMotorBackend::receive_ack(int timeout_ms) {
    uint8_t command = 0;
    uint8_t size = 0;
    uint8_t payload[32]{};
    if (receive_frame(command, payload, size, timeout_ms))
        return command == kRspAck;
    std::fprintf(stderr, "TENNIS_ERROR motor controller ACK timed out\n");
    return false;
}

bool UartMotorBackend::receive_frame(uint8_t &command, uint8_t *payload,
                                     uint8_t &size, int timeout_ms) {
    uint8_t byte = 0;
    int remaining = timeout_ms;
    while (remaining > 0) {
        const int slice = std::min(remaining, 10);
        if (!serial_.read_byte(byte, slice)) {
            remaining -= slice;
            continue;
        }
        if (byte != kSof0 || !serial_.read_byte(byte, 10) || byte != kSof1)
            continue;
        if (!serial_.read_byte(command, 20) || !serial_.read_byte(size, 20) ||
            size > 32)
            return false;
        for (uint8_t i = 0; i < size; ++i) {
            if (!serial_.read_byte(payload[i], 20)) return false;
        }
        uint8_t received = 0;
        return serial_.read_byte(received, 20) &&
               received == checksum(command, size, payload);
    }
    return false;
}

bool UartMotorBackend::drive(int left, int right) {
    const auto left_speed = static_cast<int16_t>(std::clamp(left, -100, 100));
    const auto right_speed = static_cast<int16_t>(std::clamp(right, -100, 100));
    uint8_t payload[4];
    put_be16(payload, static_cast<uint16_t>(left_speed));
    put_be16(payload + 2, static_cast<uint16_t>(right_speed));
    return send_command(kCmdSetSpeeds, payload, sizeof(payload), false);
}

bool UartMotorBackend::brake() {
    return stop();
}

bool UartMotorBackend::standby() {
    return stop();
}

TelemetryResult UartMotorBackend::read_wheel_rpm(WheelRpm &rpm) {
    const uint8_t all_motors = 2;
    serial_.flush_input();
    if (!send_command(kCmdGetRpm, &all_motors, 1, false))
        return TelemetryResult::Failed;

    bool have_left = false;
    bool have_right = false;
    for (int response = 0; response < 2; ++response) {
        uint8_t command = 0;
        uint8_t size = 0;
        uint8_t payload[32]{};
        if (!receive_frame(command, payload, size, 150) ||
            command != kRspRpmData || size < 3)
            return TelemetryResult::Failed;
        const int value = get_be16(payload + 1);
        if (payload[0] == 0) {
            rpm.left = value;
            have_left = true;
        } else if (payload[0] == 1) {
            rpm.right = value;
            have_right = true;
        }
    }
    return have_left && have_right ? TelemetryResult::Sample
                                   : TelemetryResult::Failed;
}

bool UartMotorBackend::stop() {
    const uint8_t all_motors = 2;
    return send_command(kCmdStop, &all_motors, 1, false);
}

} // namespace tennis
