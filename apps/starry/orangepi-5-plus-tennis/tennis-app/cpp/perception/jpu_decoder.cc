// SPDX-License-Identifier: Apache-2.0
//
// MJPEG -> NV12 hardware decode via librockchip_mpp, driving the RK3588 JPU
// through the StarryOS /dev/mpp_service node. See jpu_decoder.h.
//
// Threaded MPP decode (the proven path): decode_put_packet on this thread, then
// a blocking decode_get_frame. MPP's internal worker thread does the HAL/HW work
// and copies the raw-pointer packet into device memory itself. On StarryOS the
// per-frame caller<->worker cond-var handoffs are what the scheduler same-CPU
// wakeup latency affects; that is addressed in the kernel, not here.

#include "jpu_decoder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "rk_mpi.h"
#include "rk_mpi_cmd.h"
#include "rk_type.h"

namespace tennis {

namespace {
// dma-heap UAPI (Linux dma-buf heaps). DMA_HEAP_IOCTL_ALLOC = _IOWR('H',0,..).
struct DmaHeapAllocData {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
constexpr unsigned long kDmaHeapIoctlAlloc = 0xC0184800ul;

// Allocate a dma-buf from /dev/dma_heap and CPU-map it ourselves. Used for MPP's
// EXTERNAL output buffer group: MPP's own dma-heap allocator hands back a CPU
// pointer that faults on StarryOS, so we own the fd + mmap and commit them.
int dma_heap_alloc(int heap_fd, size_t size, int *out_fd, void **out_ptr) {
    DmaHeapAllocData d;
    std::memset(&d, 0, sizeof(d));
    d.len = size;
    d.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, kDmaHeapIoctlAlloc, &d) != 0) return -1;
    void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, static_cast<int>(d.fd), 0);
    if (p == MAP_FAILED) {
        close(static_cast<int>(d.fd));
        return -1;
    }
    *out_fd = static_cast<int>(d.fd);
    *out_ptr = p;
    return 0;
}

inline MppCtx ctx_of(void *p) { return reinterpret_cast<MppCtx>(p); }
inline MppApi *mpi_of(void *p) { return reinterpret_cast<MppApi *>(p); }
inline double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1.0e6;
}
} // namespace

JpuDecoder::~JpuDecoder() { deinit(); }

bool JpuDecoder::init() {
    MppCtx ctx = nullptr;
    MppApi *mpi = nullptr;
    if (mpp_create(&ctx, &mpi) != MPP_OK || ctx == nullptr || mpi == nullptr) {
        std::fprintf(stderr, "TENNIS_JPU mpp_create failed\n");
        return false;
    }

    // No-thread (synchronous) decode. MPP's worker-thread handoff never delivers a
    // frame on StarryOS (decode_get_frame times out on every packet — a real
    // cross-thread bug independent of the dma-buf mapping). Running the parser +
    // HAL inline via mpi->decode() works. Must be set before mpp_init.
    mpi->control(ctx, MPP_SET_DISABLE_THREAD, nullptr);

    // Parser split mode off: each camera buffer is one complete JPEG (one frame).
    RK_U32 split = 0;
    mpi->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &split);

    if (mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG) != MPP_OK) {
        std::fprintf(stderr, "TENNIS_JPU mpp_init(MJPEG) failed\n");
        mpp_destroy(ctx);
        return false;
    }

    // NV12 output so the RGA can do a single NV12->RGB888 CSC into the NPU input.
    MppFrameFormat fmt = MPP_FMT_YUV420SP;
    mpi->control(ctx, MPP_DEC_SET_OUTPUT_FORMAT, &fmt);
    // Emit each decoded frame immediately (no reorder buffering for MJPEG).
    RK_U32 imm = 1;
    mpi->control(ctx, MPP_DEC_SET_IMMEDIATE_OUT, &imm);
    // Block on input-queue space; block (bounded) on output so decode_get_frame
    // returns the instant the frame is ready (no busy-wait). 50ms ceiling bounds
    // a truncated/garbage JPEG that yields no frame (-> MPP_ERR_TIMEOUT, dropped).
    RK_S64 in_to = -1;
    RK_S64 out_to = 50; // ms
    mpi->control(ctx, MPP_SET_INPUT_TIMEOUT, &in_to);
    mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &out_to);

    // Input packets must be dma-buf-backed: MPP only parses/decodes a packet that
    // owns an MppBuffer (mpp_dec_normal.c: `if (mpp_packet_get_buffer(packet))`).
    // For a raw-pointer packet MPP must copy into an internal stream buffer first,
    // and that internal copy never completes on StarryOS (get_frame times out on
    // every frame -> silent CPU fallback). Allocating the input packet from a
    // dma-heap group ourselves makes the packet buffer-backed up front.
    {
        MppBufferGroup ig = nullptr;
        if (mpp_buffer_group_get_internal(&ig, MPP_BUFFER_TYPE_DMA_HEAP) == MPP_OK) {
            in_grp_ = ig;
        } else {
            std::fprintf(stderr, "TENNIS_JPU input buffer_group_get_internal failed (raw packets)\n");
        }
    }

    ctx_ = ctx;
    mpi_ = mpi;
    std::fprintf(stderr, "TENNIS_JPU decoder ready (MJPEG->NV12 via /dev/mpp_service)\n");
    return true;
}

void JpuDecoder::release_held_frame() {
    if (held_frame_ != nullptr) {
        MppFrame f = reinterpret_cast<MppFrame>(held_frame_);
        mpp_frame_deinit(&f);
        held_frame_ = nullptr;
    }
}

void JpuDecoder::deinit() {
    release_held_frame();
    if (ctx_ != nullptr) {
        mpi_of(mpi_)->reset(ctx_of(ctx_));
        mpp_destroy(ctx_of(ctx_));
        ctx_ = nullptr;
        mpi_ = nullptr;
    }
    if (frm_grp_ != nullptr) {
        mpp_buffer_group_put(reinterpret_cast<MppBufferGroup>(frm_grp_));
        frm_grp_ = nullptr;
    }
    if (in_grp_ != nullptr) {
        mpp_buffer_group_put(reinterpret_cast<MppBufferGroup>(in_grp_));
        in_grp_ = nullptr;
    }
    if (heap_fd_ >= 0) {
        close(heap_fd_);
        heap_fd_ = -1;
    }
}

bool JpuDecoder::decode(const uint8_t *data, size_t len, JpuFrame &out) {
    if (ctx_ == nullptr || data == nullptr || len == 0) return false;
    release_held_frame();

    MppCtx ctx = ctx_of(ctx_);
    MppApi *mpi = mpi_of(mpi_);

    const double t_put0 = now_ms();
    MppPacket packet = nullptr;
    MppBuffer in_buf = nullptr;
    if (in_grp_ != nullptr &&
        mpp_buffer_get(reinterpret_cast<MppBufferGroup>(in_grp_), &in_buf, len) == MPP_OK &&
        in_buf != nullptr) {
        // dma-buf-backed input packet: copy the JPEG into device memory the JPU
        // can DMA from. MPP then parses/decodes it directly (no failing internal
        // stream-buffer copy). MPP's own mpp_buffer_get_ptr() yields no CPU
        // mapping on StarryOS (NULL -> the previous memcpy SIGSEGV'd), so map the
        // dma-buf fd ourselves — the kernel /dev/dma_heap mmap works.
        // MPP's own mpp_buffer_get_ptr() pointer faults on write in the dynamic
        // app on StarryOS, so map the dma-buf fd ourselves with an explicit
        // PROT_READ|PROT_WRITE/MAP_SHARED mmap (validated to work) and copy there.
        const int bfd = mpp_buffer_get_fd(in_buf);
        const size_t msz = (len + 0xFFFUL) & ~0xFFFUL;
        void *my_map = mmap(nullptr, msz, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);
        if (my_map == MAP_FAILED) {
            mpp_buffer_put(in_buf);
            return false;
        }
        std::memcpy(my_map, data, len);
        munmap(my_map, msz);
        if (mpp_packet_init_with_buffer(&packet, in_buf) != MPP_OK || packet == nullptr) {
            mpp_buffer_put(in_buf);
            return false;
        }
        mpp_packet_set_length(packet, len);
    } else {
        // Fallback: raw-pointer packet (works on Linux; times out on StarryOS).
        if (mpp_packet_init(&packet, const_cast<uint8_t *>(data), len) != MPP_OK ||
            packet == nullptr) {
            return false;
        }
        mpp_packet_set_pos(packet, const_cast<uint8_t *>(data));
        mpp_packet_set_length(packet, len);
    }
    const double t_put1 = now_ms();

    // Canonical no-thread loop (mpi_dec_nt_test): feed the SAME packet to
    // mpi->decode() until it is fully consumed (length == 0). The first call for a
    // new geometry yields an info-change frame (commit the output group, re-loop);
    // a later call yields the decoded NV12 frame.
    bool ok = false;
    for (int guard = 0; guard < 16; ++guard) {
        MppFrame frame = nullptr;
        MPP_RET ret = mpi->decode(ctx, packet, &frame);
        if (frame != nullptr) {
            if (mpp_frame_get_info_change(frame)) {
                size_t buf_size = mpp_frame_get_buf_size(frame);
                if (frm_grp_ == nullptr) {
                    // Internal NV12 output group; MPP's own CPU mapping of these is
                    // now writable (kernel DmaBufFile O_RDWR fix).
                    MppBufferGroup grp = nullptr;
                    if (mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DMA_HEAP) != MPP_OK) {
                        std::fprintf(stderr, "TENNIS_JPU output buffer_group_get_internal failed\n");
                        mpp_frame_deinit(&frame);
                        break;
                    }
                    frm_grp_ = grp;
                }
                mpp_buffer_group_limit_config(reinterpret_cast<MppBufferGroup>(frm_grp_), buf_size, 8);
                mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, frm_grp_);
                mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
                mpp_frame_deinit(&frame);
                continue;
            }
            MppBuffer buf = mpp_frame_get_buffer(frame);
            RK_U32 err = mpp_frame_get_errinfo(frame) | mpp_frame_get_discard(frame);
            if (buf != nullptr && !err) {
                out.fd = mpp_buffer_get_fd(buf);
                out.width = static_cast<int>(mpp_frame_get_width(frame));
                out.height = static_cast<int>(mpp_frame_get_height(frame));
                out.hor_stride = static_cast<int>(mpp_frame_get_hor_stride(frame));
                out.ver_stride = static_cast<int>(mpp_frame_get_ver_stride(frame));
                out.put_ms = t_put1 - t_put0;
                out.getframe_ms = now_ms() - t_put1;
                held_frame_ = frame; // keep the dma-buf alive until the next decode()
                ok = (out.fd >= 0 && out.width > 0 && out.height > 0);
            } else {
                mpp_frame_deinit(&frame);
            }
        }
        if (ok) break;
        if (mpp_packet_get_length(packet) == 0) break; // packet fully consumed
    }
    mpp_packet_deinit(&packet);
    if (in_buf != nullptr) mpp_buffer_put(in_buf);
    return ok;
}

} // namespace tennis
