#pragma once
// Publish a downscaled camera frame to a tmpfs file for the STARRY//SIGNAL
// dashboard viewport (see apps/.../dashboard/dashboard.cpp CameraFeed). Opt-in,
// low-fps, and off the control-loop critical path. Format = exactly what the
// dashboard reads: an 8-byte header 'C','F', u16 seq, u16 w, u16 h (little-endian),
// then w*h*4 RGBA8888. Written atomically (write .tmp + rename) so the reader
// never sees a torn frame. Source is RGB888 (frame_to_image output); the downscale
// is a cheap nearest-neighbour of an already-decoded CPU frame.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

inline void publish_camera_frame(const std::string& path, const uint8_t* rgb,
                                 int sw, int sh, int seq, int ow = 320, int oh = 240) {
    if (!rgb || sw <= 0 || sh <= 0 || ow <= 0 || oh <= 0 || path.empty()) return;
    std::vector<uint8_t> out(size_t(8) + size_t(ow) * size_t(oh) * 4);
    uint8_t* h = out.data();
    h[0] = 'C'; h[1] = 'F';
    h[2] = uint8_t(seq & 0xff);  h[3] = uint8_t((seq >> 8) & 0xff);
    h[4] = uint8_t(ow & 0xff);   h[5] = uint8_t((ow >> 8) & 0xff);
    h[6] = uint8_t(oh & 0xff);   h[7] = uint8_t((oh >> 8) & 0xff);
    uint8_t* px = out.data() + 8;
    for (int y = 0; y < oh; ++y) {
        const uint8_t* srow = rgb + size_t(y * sh / oh) * size_t(sw) * 3;
        uint8_t* drow = px + size_t(y) * size_t(ow) * 4;
        for (int x = 0; x < ow; ++x) {
            const uint8_t* s = srow + size_t(x * sw / ow) * 3;
            drow[x * 4 + 0] = s[0]; drow[x * 4 + 1] = s[1];
            drow[x * 4 + 2] = s[2]; drow[x * 4 + 3] = 0xff;  // RGB888 -> RGBA8888
        }
    }
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (ok) std::rename(tmp.c_str(), path.c_str());  // atomic swap in
    else std::remove(tmp.c_str());
}
