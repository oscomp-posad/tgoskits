// SPDX-License-Identifier: Apache-2.0
//
// Differential-drive motor backend abstraction. The steering->wheel mapping
// lives in the control layer (state_machine); the dead-zone/scale mapping lives
// in the Motor facade. A backend only sees the final per-wheel command, so a
// real UART driver can drop in later without touching control or perception.
#pragma once

namespace tennis {

struct WheelRpm {
    int left = 0;
    int right = 0;
};

enum class TelemetryResult {
    Unsupported,
    Sample,
    Failed,
};

// Backend contract: signed speeds in [-100, 100], positive = forward.
class MotorBackend {
public:
    virtual ~MotorBackend() = default;
    virtual bool drive(int left, int right) = 0; // set per-wheel speed
    virtual bool brake() = 0;                    // actively lock both wheels
    virtual bool standby() = 0;                  // coast / outputs off
    virtual TelemetryResult read_wheel_rpm(WheelRpm &) {
        return TelemetryResult::Unsupported;
    }
};

// Virtual backend: emits structured, machine-readable command lines and touches
// no hardware.
class TraceMotorBackend final : public MotorBackend {
public:
    bool drive(int left, int right) override;
    bool brake() override;
    bool standby() override;
};

} // namespace tennis
