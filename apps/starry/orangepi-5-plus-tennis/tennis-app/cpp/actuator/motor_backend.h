// SPDX-License-Identifier: Apache-2.0
//
// Differential-drive motor backend abstraction. The steering->wheel mapping
// lives in the control layer (state_machine); the dead-zone/scale mapping lives
// in the Motor facade. A backend only sees the final per-wheel command, so a
// real UART driver can drop in later without touching control or perception.
#pragma once

namespace tennis {

// Backend contract: signed speeds in [-100, 100], positive = forward.
class MotorBackend {
public:
    virtual ~MotorBackend() = default;
    virtual void drive(int left, int right) = 0; // set per-wheel speed
    virtual void brake() = 0;                     // actively lock both wheels
    virtual void standby() = 0;                   // coast / outputs off
};

// Virtual backend: emits structured, machine-readable command lines and touches
// no hardware. This is the default and the only backend built by the first PR.
class TraceMotorBackend final : public MotorBackend {
public:
    void drive(int left, int right) override;
    void brake() override;
    void standby() override;
};

} // namespace tennis
