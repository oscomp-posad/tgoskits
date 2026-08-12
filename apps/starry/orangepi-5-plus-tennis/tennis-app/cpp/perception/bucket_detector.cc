// SPDX-License-Identifier: Apache-2.0
#include "bucket_detector.h"

#include <algorithm>

namespace tennis {

// Integer RGB->HSV. H in [0,360), S/V in [0,255]. Matches the reference mapping.
static void rgb2hsv(int r, int g, int b, int &h, int &s, int &v) {
    const int cmax = std::max(r, std::max(g, b));
    const int cmin = std::min(r, std::min(g, b));
    const int delta = cmax - cmin;
    v = cmax;
    s = (cmax == 0) ? 0 : (delta * 255 / cmax);
    if (delta == 0) {
        h = 0;
        return;
    }
    int hh;
    if (cmax == r) {
        hh = 60 * (((g - b) * 1000 / delta)) / 1000;
    } else if (cmax == g) {
        hh = 60 * (((b - r) * 1000 / delta) + 2000) / 1000;
    } else {
        hh = 60 * (((r - g) * 1000 / delta) + 4000) / 1000;
    }
    hh %= 360;
    if (hh < 0) hh += 360;
    h = hh;
}

void BucketDetector::ensure_capacity(int w, int h) {
    const int n = w * h;
    if (cap_ == n) return;
    mask_.assign(n, 0);
    parent_.assign(n, -1);
    count_.assign(n, 0);
    minx_.assign(n, 0);
    miny_.assign(n, 0);
    maxx_.assign(n, 0);
    maxy_.assign(n, 0);
    cap_ = n;
}

int BucketDetector::find(int x) {
    while (parent_[x] != x) {
        parent_[x] = parent_[parent_[x]]; // path halving
        x = parent_[x];
    }
    return x;
}

void BucketDetector::detect(const image_buffer_t *img, BucketObs &out) {
    out = BucketObs{};
    if (img == nullptr || img->virt_addr == nullptr ||
        img->format != IMAGE_FORMAT_RGB888) {
        return;
    }
    const int w = img->width;
    const int h = img->height;
    if (w <= 0 || h <= 0) return;
    const int stride = img->width_stride > 0 ? img->width_stride : w * 3;
    ensure_capacity(w, h);

    const unsigned char *p = img->virt_addr;

    // Pass 0: build the red mask and initialise union-find parents.
    for (int y = 0; y < h; ++y) {
        const unsigned char *row = p + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            const int idx = y * w + x;
            const int r = row[x * 3 + 0];
            const int g = row[x * 3 + 1];
            const int b = row[x * 3 + 2];
            int hh, ss, vv;
            rgb2hsv(r, g, b, hh, ss, vv);
            const bool is_red = ss >= cfg_.hsv_s_min && vv >= cfg_.hsv_v_min &&
                                (hh <= cfg_.hsv_h_lo || hh >= cfg_.hsv_h_hi);
            mask_[idx] = is_red ? 1 : 0;
            parent_[idx] = is_red ? idx : -1;
        }
    }

    // Pass 1: union with up and left neighbours (4-connectivity).
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int idx = y * w + x;
            if (!mask_[idx]) continue;
            if (x > 0 && mask_[idx - 1]) {
                int a = find(idx), bb = find(idx - 1);
                if (a != bb) parent_[a] = bb;
            }
            if (y > 0 && mask_[idx - w]) {
                int a = find(idx), bb = find(idx - w);
                if (a != bb) parent_[a] = bb;
            }
        }
    }

    // Pass 2: accumulate per-root bounding box and pixel count.
    std::fill(count_.begin(), count_.begin() + cap_, 0);
    int best_root = -1;
    int best_count = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int idx = y * w + x;
            if (!mask_[idx]) continue;
            const int root = find(idx);
            if (count_[root] == 0) {
                minx_[root] = maxx_[root] = x;
                miny_[root] = maxy_[root] = y;
            } else {
                if (x < minx_[root]) minx_[root] = x;
                if (x > maxx_[root]) maxx_[root] = x;
                if (y < miny_[root]) miny_[root] = y;
                if (y > maxy_[root]) maxy_[root] = y;
            }
            const int c = ++count_[root];
            if (c > best_count) {
                best_count = c;
                best_root = root;
            }
        }
    }

    if (best_root < 0 || best_count < cfg_.bucket_min_area) return;

    const int bx = minx_[best_root];
    const int by = miny_[best_root];
    const int bw = maxx_[best_root] - minx_[best_root] + 1;
    const int bh = maxy_[best_root] - miny_[best_root] + 1;
    out.found = true;
    out.x = bx;
    out.y = by;
    out.w = bw;
    out.h = bh;
    out.cx = static_cast<float>(bx) + static_cast<float>(bw) / 2.f;
    out.cy = static_cast<float>(by) + static_cast<float>(bh) / 2.f;
    out.area_ratio = static_cast<float>(best_count) /
                     (static_cast<float>(w) * static_cast<float>(h));
}

} // namespace tennis
