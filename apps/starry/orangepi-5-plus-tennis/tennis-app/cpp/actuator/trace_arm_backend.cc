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

void TraceArmBackend::grab() { std::printf("TENNIS_ARM grab\n"); }

void TraceArmBackend::release() { std::printf("TENNIS_ARM release\n"); }

void TraceArmBackend::ready() { std::printf("TENNIS_ARM ready\n"); }

} // namespace tennis
