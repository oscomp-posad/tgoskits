// SPDX-License-Identifier: Apache-2.0
#include "actuator_factory.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>

#include "app_options.h"
#include "pwm_motor_backend.h"
#include "uart_arm_backend.h"
#include "uart_motor_backend.h"

namespace tennis {
namespace {

class DeadZoneMotorBackend final : public MotorBackend {
public:
    DeadZoneMotorBackend(std::unique_ptr<MotorBackend> backend, int minimum)
        : backend_(std::move(backend)), minimum_(std::clamp(minimum, 1, 100)) {}

    bool drive(int left, int right) override {
        return backend_->drive(map(left), map(right));
    }
    bool brake() override { return backend_->brake(); }
    bool standby() override { return backend_->standby(); }
    TelemetryResult read_wheel_rpm(WheelRpm &rpm) override {
        return backend_->read_wheel_rpm(rpm);
    }

private:
    int map(int speed) const {
        speed = std::clamp(speed, -100, 100);
        if (speed == 0) return 0;
        const int sign = speed > 0 ? 1 : -1;
        const int magnitude = std::abs(speed);
        return sign * std::max(magnitude, minimum_);
    }

    std::unique_ptr<MotorBackend> backend_;
    int minimum_;
};

} // namespace

bool uses_virtual_actuators(const Options &options) {
    return options.motor_backend == "virtual" && options.arm_backend == "virtual";
}

bool make_actuators(const Options &options, Actuators &actuators) {
    if (options.motor_backend == "virtual") {
        actuators.motor = std::make_unique<TraceMotorBackend>();
    }
#ifdef TENNIS_HOST_DRYRUN
    else {
        std::fprintf(stderr,
                     "TENNIS_ERROR real actuators require the board build\n");
        return false;
    }
#else
    else if (options.motor_backend == "pwm") {
        const std::string device = options.motor_device.empty()
            ? "pwm:/sys/class/pwm/pwmchip0,/sys/class/pwm/pwmchip1,"
              "/sys/class/pwm/pwmchip4,/sys/class/pwm/pwmchip5"
            : options.motor_device;
        auto backend = std::make_unique<PwmMotorBackend>(device);
        if (!backend->ready()) return false;
        actuators.motor = std::make_unique<DeadZoneMotorBackend>(
            std::move(backend), options.cfg.motor_min_speed);
    } else if (options.motor_backend == "uart") {
        const std::string device =
            options.motor_device.empty() ? "/dev/ttyS6" : options.motor_device;
        auto backend = std::make_unique<UartMotorBackend>(device);
        if (!backend->ready()) return false;
        actuators.motor = std::make_unique<DeadZoneMotorBackend>(
            std::move(backend), options.cfg.motor_min_speed);
    } else {
        std::fprintf(stderr, "TENNIS_ERROR unsupported motor backend: %s\n",
                     options.motor_backend.c_str());
        return false;
    }
#endif

    if (options.arm_backend == "virtual") {
        actuators.arm = std::make_unique<TraceArmBackend>();
    }
#ifdef TENNIS_HOST_DRYRUN
    else {
        std::fprintf(stderr,
                     "TENNIS_ERROR real actuators require the board build\n");
        return false;
    }
#else
    else if (options.arm_backend == "uart") {
        const std::string device =
            options.arm_device.empty() ? "/dev/ttyS3" : options.arm_device;
        auto backend = std::make_unique<UartArmBackend>(device);
        if (!backend->is_ready()) return false;
        actuators.arm = std::move(backend);
    } else {
        std::fprintf(stderr, "TENNIS_ERROR unsupported arm backend: %s\n",
                     options.arm_backend.c_str());
        return false;
    }
#endif
    return true;
}

} // namespace tennis
