// SPDX-License-Identifier: Apache-2.0

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <poll.h>
#include <pty.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "actuator/uart_arm_backend.h"
#include "actuator/uart_motor_backend.h"

namespace {

struct PtyPair {
    PtyPair() {
        char path[128]{};
        if (openpty(&master, &slave, path, nullptr, nullptr) == 0)
            device = path;
    }

    ~PtyPair() {
        if (master >= 0) close(master);
        if (slave >= 0) close(slave);
    }

    int master = -1;
    int slave = -1;
    std::string device;
};

bool read_exact(int fd, void *data, size_t size, int timeout_ms) {
    auto *bytes = static_cast<uint8_t *>(data);
    size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (offset < size) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) return false;
        pollfd descriptor{fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, static_cast<int>(remaining.count()));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return false;
        const ssize_t received = read(fd, bytes + offset, size - offset);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return false;
        offset += static_cast<size_t>(received);
    }
    return true;
}

uint8_t checksum(uint8_t command, const std::vector<uint8_t> &payload) {
    uint8_t value = command ^ static_cast<uint8_t>(payload.size());
    for (uint8_t byte : payload) value ^= byte;
    return value;
}

std::vector<uint8_t> frame(uint8_t command,
                           const std::vector<uint8_t> &payload = {}) {
    std::vector<uint8_t> result{0xaa, 0x55, command,
                                static_cast<uint8_t>(payload.size())};
    result.insert(result.end(), payload.begin(), payload.end());
    result.push_back(checksum(command, payload));
    return result;
}

bool read_frame(int fd, std::vector<uint8_t> &result) {
    uint8_t header[4];
    if (!read_exact(fd, header, sizeof(header), 2000)) return false;
    result.assign(header, header + sizeof(header));
    std::vector<uint8_t> tail(static_cast<size_t>(header[3]) + 1);
    if (!read_exact(fd, tail.data(), tail.size(), 2000)) return false;
    result.insert(result.end(), tail.begin(), tail.end());
    return true;
}

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

bool motor_protocol_matches_esp32_controller() {
    PtyPair pty;
    if (!expect(pty.master >= 0, "open motor PTY")) return false;

    std::vector<std::vector<uint8_t>> received;
    std::string emulator_error;
    std::thread emulator([&] {
        for (int i = 0; i < 6; ++i) {
            std::vector<uint8_t> command;
            if (!read_frame(pty.master, command)) {
                emulator_error = "timed out reading motor frame";
                return;
            }
            received.push_back(std::move(command));
            if (i < 2) {
                const auto ack = frame(0x80);
                if (write(pty.master, ack.data(), ack.size()) !=
                    static_cast<ssize_t>(ack.size())) {
                    emulator_error = "failed to write motor ACK";
                    return;
                }
            }
        }
    });

    bool calls_succeeded = true;
    {
        tennis::UartMotorBackend motor(pty.device);
        calls_succeeded = motor.ready() && motor.drive(120, -101) &&
                          motor.brake() && motor.standby();
    }
    emulator.join();

    if (!expect(calls_succeeded, "motor backend commands succeed")) return false;
    if (!expect(emulator_error.empty(), emulator_error.c_str())) return false;

    const std::vector<std::vector<uint8_t>> expected{
        frame(0x01),
        frame(0x02, {0x12, 0x48, 0x4e, 0x20}),
        frame(0x13, {0x00, 0x64, 0xff, 0x9c}),
        frame(0x11, {0x02}),
        frame(0x11, {0x02}),
        frame(0x11, {0x02}),
    };
    return expect(received == expected,
                  "motor frames match INIT/CONFIG/SET_SPEEDS/STOP protocol");
}

bool motor_rpm_protocol_matches_esp32_controller() {
    PtyPair pty;
    if (!expect(pty.master >= 0, "open RPM motor PTY")) return false;

    std::vector<std::vector<uint8_t>> received;
    std::string emulator_error;
    std::thread emulator([&] {
        for (int i = 0; i < 4; ++i) {
            std::vector<uint8_t> command;
            if (!read_frame(pty.master, command)) {
                emulator_error = "timed out reading RPM motor frame";
                return;
            }
            received.push_back(command);
            if (i < 2) {
                const auto ack = frame(0x80);
                if (write(pty.master, ack.data(), ack.size()) !=
                    static_cast<ssize_t>(ack.size())) {
                    emulator_error = "failed to write RPM motor ACK";
                    return;
                }
            } else if (i == 2) {
                const auto left = frame(0x90, {0, 0, 36});
                const auto right = frame(0x90, {1, 0, 35});
                if (write(pty.master, left.data(), left.size()) !=
                        static_cast<ssize_t>(left.size()) ||
                    write(pty.master, right.data(), right.size()) !=
                        static_cast<ssize_t>(right.size())) {
                    emulator_error = "failed to write RPM data";
                    return;
                }
            }
        }
    });

    tennis::WheelRpm rpm;
    bool succeeded = false;
    {
        tennis::UartMotorBackend motor(pty.device);
        succeeded = motor.ready() &&
                    motor.read_wheel_rpm(rpm) == tennis::TelemetryResult::Sample;
    }
    emulator.join();
    if (!expect(succeeded, "RPM query must succeed")) return false;
    if (!expect(emulator_error.empty(), emulator_error.c_str())) return false;
    return expect(rpm.left == 36 && rpm.right == 35,
                  "RPM response must decode signed big-endian wheel values") &&
           expect(received.size() == 4 && received[2] == frame(0x20, {2}),
                  "RPM query must use command 0x20 with both motors");
}

bool motor_drive_can_preempt_an_rpm_wait() {
    PtyPair pty;
    if (!expect(pty.master >= 0, "open concurrent motor PTY")) return false;

    std::vector<std::vector<uint8_t>> received;
    std::string emulator_error;
    std::mutex stage_mutex;
    std::condition_variable stage_changed;
    bool rpm_request_seen = false;

    std::thread emulator([&] {
        for (int i = 0; i < 2; ++i) {
            std::vector<uint8_t> command;
            if (!read_frame(pty.master, command)) {
                emulator_error = "timed out reading concurrent motor init";
                return;
            }
            received.push_back(command);
            const auto ack = frame(0x80);
            if (write(pty.master, ack.data(), ack.size()) !=
                static_cast<ssize_t>(ack.size())) {
                emulator_error = "failed to write concurrent motor ACK";
                return;
            }
        }

        std::vector<uint8_t> rpm_command;
        if (!read_frame(pty.master, rpm_command)) {
            emulator_error = "timed out reading concurrent RPM request";
            return;
        }
        received.push_back(rpm_command);
        {
            std::lock_guard<std::mutex> lock(stage_mutex);
            rpm_request_seen = true;
        }
        stage_changed.notify_one();

        // Hold the telemetry response until the control path has sent a new
        // drive frame. The RPM worker must not own the UART write path while it
        // waits for these responses.
        std::vector<uint8_t> drive_command;
        if (!read_frame(pty.master, drive_command)) {
            emulator_error = "drive command was blocked behind RPM response";
            return;
        }
        received.push_back(drive_command);

        const auto left = frame(0x90, {0, 0, 36});
        const auto right = frame(0x90, {1, 0, 35});
        if (write(pty.master, left.data(), left.size()) !=
                static_cast<ssize_t>(left.size()) ||
            write(pty.master, right.data(), right.size()) !=
                static_cast<ssize_t>(right.size())) {
            emulator_error = "failed to write concurrent RPM data";
            return;
        }

        std::vector<uint8_t> stop_command;
        if (!read_frame(pty.master, stop_command)) {
            emulator_error = "timed out reading concurrent motor stop";
            return;
        }
        received.push_back(stop_command);
    });

    tennis::TelemetryResult rpm_result = tennis::TelemetryResult::Failed;
    tennis::WheelRpm rpm;
    bool drive_succeeded = false;
    {
        tennis::UartMotorBackend motor(pty.device);
        if (!expect(motor.ready(), "concurrent motor backend ready")) {
            emulator.join();
            return false;
        }
        std::thread rpm_reader([&] { rpm_result = motor.read_wheel_rpm(rpm); });
        {
            std::unique_lock<std::mutex> lock(stage_mutex);
            (void)stage_changed.wait_for(lock, std::chrono::seconds(2),
                                         [&] { return rpm_request_seen; });
        }
        drive_succeeded = motor.drive(30, -30);
        rpm_reader.join();
    }
    emulator.join();

    if (!expect(emulator_error.empty(), emulator_error.c_str())) return false;
    if (!expect(drive_succeeded, "drive succeeds during RPM wait")) return false;
    if (!expect(rpm_result == tennis::TelemetryResult::Sample,
                "RPM query completes after concurrent drive"))
        return false;
    const std::vector<std::vector<uint8_t>> expected{
        frame(0x01),
        frame(0x02, {0x12, 0x48, 0x4e, 0x20}),
        frame(0x20, {0x02}),
        frame(0x13, {0x00, 0x1e, 0xff, 0xe2}),
        frame(0x11, {0x02}),
    };
    return expect(received == expected,
                  "RPM and drive frames remain complete and ordered");
}

bool arm_protocol_matches_calibrated_sequence() {
    PtyPair pty;
    if (!expect(pty.master >= 0, "open arm PTY")) return false;

    const std::string expected =
        "#000P1648T0500!#001P1351T0500!#002P1981T0500!"
        "#002P1981T0500!"
        "#000P2203T0500!#001P1166T0500!#002P1981T0500!"
        "#002P1403T0500!"
        "#002PRAD!"
        "#000P1648T0500!#001P1351T0500!#002P1403T0500!"
        "#002PRAD!"
        "#002P1981T0500!";
    const std::string query = "#002PRAD!";
    const std::string position = "#002P1647!";
    std::string received;
    std::string emulator_error;
    std::thread emulator([&] {
        while (received.size() < expected.size()) {
            char byte = 0;
            if (!read_exact(pty.master, &byte, 1, 2000)) {
                emulator_error = "timed out reading arm command";
                return;
            }
            received.push_back(byte);
            if (received.size() >= query.size() &&
                received.compare(received.size() - query.size(), query.size(),
                                 query) == 0 &&
                write(pty.master, position.data(), position.size()) !=
                    static_cast<ssize_t>(position.size())) {
                emulator_error = "failed to write arm position response";
                return;
            }
        }
    });

    tennis::GrabResult grab_result = tennis::GrabResult::Error;
    bool calls_succeeded = true;
    {
        tennis::UartArmBackend arm(pty.device);
        calls_succeeded = arm.is_ready() && arm.ready();
        grab_result = arm.grab();
        calls_succeeded = calls_succeeded && arm.release();
    }
    emulator.join();

    if (!expect(calls_succeeded, "arm origin and put commands succeed"))
        return false;
    if (!expect(grab_result == tennis::GrabResult::Captured,
                "two position samples above P1530 confirm capture"))
        return false;
    if (!expect(emulator_error.empty(), emulator_error.c_str())) return false;
    return expect(received == expected,
                  "arm commands match calibrated origin/pick/put sequence");
}

bool arm_ball_lost_after_lift_is_reported() {
    PtyPair pty;
    if (!expect(pty.master >= 0, "open lost-ball arm PTY")) return false;

    const std::string expected =
        "#002P1981T0500!"
        "#000P2203T0500!#001P1166T0500!#002P1981T0500!"
        "#002P1403T0500!"
        "#002PRAD!"
        "#000P1648T0500!#001P1351T0500!#002P1403T0500!"
        "#002PRAD!"
        "#002P1981T0500!";
    const std::string query = "#002PRAD!";
    const std::string held_position = "#002P1647!";
    const std::string empty_position = "#002P1416!";
    std::string received;
    std::string emulator_error;
    std::thread emulator([&] {
        int query_count = 0;
        while (received.size() < expected.size()) {
            char byte = 0;
            if (!read_exact(pty.master, &byte, 1, 2000)) {
                emulator_error = "timed out reading lost-ball command";
                return;
            }
            received.push_back(byte);
            if (received.size() >= query.size() &&
                received.compare(received.size() - query.size(), query.size(),
                                 query) == 0) {
                const std::string &position =
                    query_count++ == 0 ? held_position : empty_position;
                if (write(pty.master, position.data(), position.size()) !=
                    static_cast<ssize_t>(position.size())) {
                    emulator_error = "failed to write arm position response";
                    return;
                }
            }
        }
    });

    tennis::GrabResult result = tennis::GrabResult::Error;
    {
        tennis::UartArmBackend arm(pty.device);
        result = arm.grab();
    }
    emulator.join();

    if (!expect(result == tennis::GrabResult::Empty,
                "a ball lost after lifting must not confirm capture"))
        return false;
    if (!expect(emulator_error.empty(), emulator_error.c_str())) return false;
    return expect(received == expected,
                  "capture is sampled before and after lifting");
}

} // namespace

int main() {
    if (!motor_protocol_matches_esp32_controller()) return 1;
    if (!motor_rpm_protocol_matches_esp32_controller()) return 1;
    if (!motor_drive_can_preempt_an_rpm_wait()) return 1;
    if (!arm_protocol_matches_calibrated_sequence()) return 1;
    if (!arm_ball_lost_after_lift_is_reported()) return 1;
    return 0;
}
