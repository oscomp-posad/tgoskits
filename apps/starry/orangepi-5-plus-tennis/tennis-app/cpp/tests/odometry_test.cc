// SPDX-License-Identifier: Apache-2.0
#include <cmath>
#include <cstdio>

#include "odometry.h"

namespace {

bool expect(bool condition, const char *message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

bool straight_line_integrates() {
    tennis::Config cfg;
    cfg.odometry_stale_ms = 500;
    tennis::OdometryTracker tracker(cfg);
    tracker.reset_anchor();
    tracker.update(60, 60, 1000000000LL);
    tracker.update(60, 60, 1100000000LL);
    const auto pose = tracker.estimate(1100000000LL);
    const double expected = 2.0 * 3.14159265358979323846 * 0.03 * 0.1;
    return expect(pose.valid, "straight-line pose must be valid") &&
           expect(std::abs(pose.x - expected) < 1e-6,
                  "equal wheel RPM must integrate forward distance") &&
           expect(std::abs(pose.y) < 1e-9 && std::abs(pose.heading) < 1e-9,
                  "equal wheel RPM must not turn");
}

bool differential_turn_integrates() {
    tennis::Config cfg;
    tennis::OdometryTracker tracker(cfg);
    tracker.reset_anchor();
    tracker.update(-60, 60, 1000000000LL);
    tracker.update(-60, 60, 1100000000LL);
    const auto pose = tracker.estimate(1100000000LL);
    return expect(pose.valid, "turning pose must be valid") &&
           expect(pose.heading > 0.20 && pose.heading < 0.22,
                  "opposite wheel RPM must integrate heading");
}

bool long_sampling_gap_does_not_bridge_motion() {
    tennis::Config cfg;
    cfg.odometry_max_gap_ms = 500;
    tennis::OdometryTracker tracker(cfg);
    tracker.reset_anchor();
    tracker.update(60, 60, 1000000000LL);
    tracker.update(60, 60, 2000000000LL);
    const auto pose = tracker.estimate(2000000000LL);
    return expect(std::abs(pose.x) < 1e-9,
                  "a long RPM gap must not integrate unknown motion") &&
           expect(!tracker.update(1000, 0, 2100000000LL),
                  "implausible RPM must be rejected");
}

} // namespace

int main() {
    if (!straight_line_integrates()) return 1;
    if (!differential_turn_integrates()) return 1;
    if (!long_sampling_gap_does_not_bridge_motion()) return 1;
    std::puts("tennis_odometry_test: OK");
    return 0;
}
