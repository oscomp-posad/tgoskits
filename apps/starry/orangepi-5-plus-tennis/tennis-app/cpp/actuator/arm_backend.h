// SPDX-License-Identifier: Apache-2.0
//
// Gripper-arm backend abstraction. The demo only needs three high-level verbs;
// the reference robot's servo poses map onto them (ready = home/open,
// grab = reach+close+lift, release = open over the bucket).
#pragma once

namespace tennis {

// Arm verbs, also used to label the per-command trace line.
enum class ArmAction {
    None,
    Grab,
    Release,
    Ready,
};

enum class GrabResult {
    Captured,
    Empty,
    Error,
};

const char *to_string(ArmAction a);

class ArmBackend {
public:
    virtual ~ArmBackend() = default;
    virtual GrabResult grab() = 0; // close gripper, verify the ball, and lift
    virtual bool release() = 0; // open gripper to drop into the bucket
    virtual bool ready() = 0;   // return to the stowed pre-grab pose
};

// Virtual backend: emits structured command lines, no hardware.
class TraceArmBackend final : public ArmBackend {
public:
    GrabResult grab() override;
    bool release() override;
    bool ready() override;
};

} // namespace tennis
