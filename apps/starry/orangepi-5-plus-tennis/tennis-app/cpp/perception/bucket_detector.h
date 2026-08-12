// SPDX-License-Identifier: Apache-2.0
//
// Red-bucket detector: a clean reimplementation of the reference HSV red-blob
// finder (the aka-rk3588 repo carries no license). It converts RGB888 -> HSV,
// builds a dual-range red mask, runs two-pass union-find connected components,
// and returns the largest blob above a minimum pixel area. All scratch buffers
// are preallocated and reused (no per-frame heap once warmed).
//
// Board-only in practice (it consumes the decoded RGB frame from the camera),
// though it has no RKNN/UVC dependency itself.
#pragma once

#include <cstdint>
#include <vector>

#include "common.h" // image_buffer_t
#include "types.h"

namespace tennis {

class BucketDetector {
public:
    explicit BucketDetector(const Config &cfg) : cfg_(cfg) {}

    // Detect the largest red blob in an RGB888 image. Fills `out`.
    void detect(const image_buffer_t *img, BucketObs &out);

private:
    void ensure_capacity(int w, int h);
    int find(int x);

    Config cfg_;
    int cap_ = 0;
    std::vector<uint8_t> mask_;
    std::vector<int> parent_;
    std::vector<int> count_;
    std::vector<int> minx_, miny_, maxx_, maxy_;
};

} // namespace tennis
