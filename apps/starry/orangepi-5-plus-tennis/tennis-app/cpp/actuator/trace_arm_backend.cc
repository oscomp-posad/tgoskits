// SPDX-License-Identifier: Apache-2.0
#include "arm_backend.h"

#include <cstdio>

namespace tennis {

const char *to_string(ArmAction a) {
    switch (a) {
    case ArmAction::None: return "none";
    case ArmAction::Grab: return "grab";
    case ArmAction::Release: return "release";
    case ArmAction::Ready: return "ready";
    }
    return "none";
}

GrabResult TraceArmBackend::grab() {
    std::printf("TENNIS_ARM grab\n");
    return GrabResult::Captured;
}

bool TraceArmBackend::release() {
    std::printf("TENNIS_ARM release\n");
    return true;
}

bool TraceArmBackend::ready() {
    std::printf("TENNIS_ARM ready\n");
    return true;
}

} // namespace tennis
