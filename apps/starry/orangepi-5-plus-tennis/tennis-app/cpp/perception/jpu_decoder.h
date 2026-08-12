// SPDX-License-Identifier: Apache-2.0
//
// Hardware MJPEG decode via the RK3588 JPU, through librockchip_mpp + the
// StarryOS /dev/mpp_service node. Decodes a complete JPEG buffer into an NV12
// (YUV420SP) frame that lives in a /dev/dma_heap dma-buf, so the RGA can read it
// by fd and convert NV12->RGB888 straight into the NPU input (zero-copy
// MJPEG->JPU->RGA->NPU).
//
// Board-only: requires librockchip_mpp; not part of the host dry-run build.
#pragma once

#include <cstddef>
#include <cstdint>

namespace tennis {

// One decoded NV12 frame. `fd` is a dma-buf fd valid until the next decode() or
// deinit(); the RGA imports it as a YCbCr_420_SP source.
struct JpuFrame {
    int fd = -1;          // dma-buf fd of the NV12 buffer
    int width = 0;        // visible width
    int height = 0;       // visible height
    int hor_stride = 0;   // luma row stride (bytes); chroma shares it (NV12)
    int ver_stride = 0;   // luma plane height; UV starts at hor_stride*ver_stride
    // Diagnostics (ms): time inside decode_put_packet vs the blocking get_frame
    // loop. On StarryOS this separates MPP-pipeline/HW-wait latency from setup.
    double put_ms = 0.0;
    double getframe_ms = 0.0;
};

class JpuDecoder {
public:
    JpuDecoder() = default;
    ~JpuDecoder();
    JpuDecoder(const JpuDecoder &) = delete;
    JpuDecoder &operator=(const JpuDecoder &) = delete;

    // Create the MPP MJPEG decoder, request NV12 output. Returns false if MPP /
    // the JPU node is unavailable (caller falls back to the CPU JPEG path).
    bool init();
    void deinit();
    bool ready() const { return ctx_ != nullptr; }

    // Decode one complete JPEG/MJPEG buffer into `out` (NV12 dma-buf). The frame
    // is held internally and stays valid until the next decode()/deinit().
    bool decode(const uint8_t *data, size_t len, JpuFrame &out);

private:
    void release_held_frame();

    void *ctx_ = nullptr;        // MppCtx
    void *mpi_ = nullptr;        // MppApi*
    void *frm_grp_ = nullptr;    // MppBufferGroup (NV12 output buffers, dma-heap)
    void *in_grp_ = nullptr;     // MppBufferGroup (dma-buf input packets, dma-heap)
    void *held_frame_ = nullptr; // MppFrame kept alive for the current JpuFrame
    int heap_fd_ = -1;           // /dev/dma_heap/system fd for external output buffers
};

} // namespace tennis
