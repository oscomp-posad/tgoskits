// SPDX-License-Identifier: Apache-2.0
#ifndef _GNU_SOURCE
#define _GNU_SOURCE // sched_getcpu (diagnostics)
#endif
#include <sched.h>

#include "tennis_detector.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "im2d.h"         // RGA: imcvtcolor / importbuffer_fd / wrapbuffer_handle
#include "image_utils.h"  // convert_image_with_letterbox, letterbox_t, get_image_size
#include "postprocess.h"  // object_detect_result_list, init/deinit_post_process (multi-class)
#include "rga.h"          // RK_FORMAT_YUYV_422 / RK_FORMAT_RGB_888
#include "rknn_api.h"     // rknn_run, rknn_set_core_mask, rknn_create_mem, ...
#include "uvc_capture.h"  // LatestFrame, yuyv_to_rgb888, frame_to_image, UVC_FRAME_FORMAT_*

// /dev/dma_heap ALLOC ioctl (mainline uapi; avoids a kernel-header dependency).
namespace {
struct dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
} // namespace
#ifndef DMA_HEAP_IOCTL_ALLOC
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)
#endif

namespace tennis {

static rknn_core_mask parse_core_mask(const std::string &s) {
    if (s == "auto") return RKNN_NPU_CORE_AUTO;
    if (s == "0") return RKNN_NPU_CORE_0;
    if (s == "1") return RKNN_NPU_CORE_1;
    if (s == "2") return RKNN_NPU_CORE_2;
    if (s == "0_1") return RKNN_NPU_CORE_0_1;
    return RKNN_NPU_CORE_0_1_2; // "0_1_2" / "all" / default: all three cores
}

static bool supported_box_channels(int channels) {
    return channels == 32 || channels == 64;
}

static bool has_single_class_raw_head(const rknn_app_context_t &ctx) {
    int paired_scores = 0;
    for (uint32_t si = 0; si < ctx.io_num.n_output; ++si) {
        const rknn_tensor_attr &score = ctx.output_attrs[si];
        if (score.n_dims != 4 || score.dims[1] != 1) continue;
        for (uint32_t bi = 0; bi < ctx.io_num.n_output; ++bi) {
            const rknn_tensor_attr &box = ctx.output_attrs[bi];
            if (box.n_dims == 4 && supported_box_channels(box.dims[1]) &&
                box.dims[2] == score.dims[2] && box.dims[3] == score.dims[3]) {
                ++paired_scores;
                break;
            }
        }
    }
    return paired_scores >= 3;
}

TennisDetector::~TennisDetector() { deinit(); }

bool TennisDetector::init(const char *model_path, const char *label_path,
                          const Config &cfg, const std::string &core_mask) {
    cfg_ = cfg;
    if (init_yolov8_model(model_path, &ctx_) != 0) return false;

    // A dedicated tennis model exposes three spatially paired score (C==1) and
    // DFL box tensors. Box C==64 means reg_max=16; C==32 means reg_max=8.
    single_class_ = has_single_class_raw_head(ctx_);

    // Only the multi-class (COCO) path needs the label table for post_process.
    if (!single_class_) {
        if (init_post_process(label_path) != 0) {
            release_yolov8_model(&ctx_);
            return false;
        }
        post_inited_ = true;
    }

    // Pin all three NPU cores (the stock image/stream binaries leave this AUTO).
    rknn_set_core_mask(ctx_.rknn_ctx, parse_core_mask(core_mask));

    // Zero-copy preprocessing: only wired for the single-class fast path (the
    // built-in decode below reads NCHW INT8 via rknn_outputs_get, unchanged). The
    // multi-class fallback keeps the legacy wrapper path. Both io-mem and RGA are
    // best-effort: if either is unavailable at runtime the detector degrades to
    // the NEON CPU conversion and/or rknn_inputs_set.
    if (single_class_) {
        // Allocate the NPU input, then try RGA. Bind it as io-mem ONLY if RGA is
        // usable -- on StarryOS the io-mem is uncached, so a CPU write into it is
        // ~4x slower; when RGA can't drive it, tear it down and use the legacy
        // cacheable-scratch + rknn_inputs_set path instead.
        bool ok = false;
        if (setup_iomem_input() && setup_rga()) {
            if (rknn_set_io_mem(ctx_.rknn_ctx, input_mem_, &native_in_attr_) == 0) {
                iomem_ = true;
                ok = true;
                // Hardware MJPEG decode (JPU) enables the MJPEG->JPU->RGA->NPU
                // zero-copy path. If it fails to init, MJPEG frames fall back to
                // the CPU libjpeg-turbo decode.
                jpu_ready_ = jpu_.init();
            } else {
                std::fprintf(stderr, "TENNIS_RGA setup: rknn_set_io_mem failed\n");
            }
        }
        if (!ok) {
            teardown_preproc(); // destroy input_mem_, release handles -> legacy path
        }
        std::printf("TENNIS_PREPROC zero_copy=%d rga=%d model=%dx%d\n",
                    iomem_ ? 1 : 0, rga_ready_ ? 1 : 0, ctx_.model_width,
                    ctx_.model_height);
    }

    inited_ = true;
    return true;
}

void TennisDetector::deinit() {
    if (!inited_) return;
    teardown_preproc();
    release_yolov8_model(&ctx_);
    if (post_inited_) {
        deinit_post_process();
        post_inited_ = false;
    }
    if (scratch_.virt_addr) {
        std::free(scratch_.virt_addr);
        scratch_ = image_buffer_t{};
    }
    inited_ = false;
}

// --- Zero-copy NPU input -----------------------------------------------------

bool TennisDetector::setup_iomem_input() {
    std::memset(&native_in_attr_, 0, sizeof(native_in_attr_));
    native_in_attr_.index = 0;
    if (rknn_query(ctx_.rknn_ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &native_in_attr_,
                   sizeof(native_in_attr_)) != RKNN_SUCC) {
        return false;
    }
    // uint8 input fuses normalize+quantize into the NPU (the model expects this).
    native_in_attr_.type = RKNN_TENSOR_UINT8;
    // Cacheable io-mem (gives a valid CPU ->virt_addr on both Linux and StarryOS;
    // non-cacheable returned an unmapped virt_addr on StarryOS -> segfault). RGA
    // (MMU-off) writes DRAM directly, so after the blit we explicitly invalidate
    // the CPU cache (rknn_mem_sync FROM_DEVICE) so DRAM is authoritative for the
    // NPU read. The io-mem BIND is deferred to init -- only done if RGA works.
    input_mem_ = rknn_create_mem(ctx_.rknn_ctx, native_in_attr_.size_with_stride);
    if (input_mem_ == nullptr) return false;
    in_wstride_ = ctx_.model_width;
    return true;
}

bool TennisDetector::setup_rga() {
    if (input_mem_ == nullptr) return false;
    const int w = ctx_.model_width, h = ctx_.model_height;
    heap_fd_ = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap_fd_ < 0) {
        std::fprintf(stderr, "TENNIS_RGA setup: open /dev/dma_heap/system errno=%d\n",
                     errno);
        return false;
    }

    stage_size_ = static_cast<size_t>(w) * static_cast<size_t>(h) * 2; // YUYV422
    dma_heap_allocation_data a;
    std::memset(&a, 0, sizeof(a));
    a.len = stage_size_;
    a.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd_, DMA_HEAP_IOCTL_ALLOC, &a) < 0) {
        std::fprintf(stderr, "TENNIS_RGA setup: DMA_HEAP_ALLOC errno=%d\n", errno);
        return false;
    }
    stage_fd_ = static_cast<int>(a.fd);

    stage_map_ = static_cast<unsigned char *>(
        mmap(nullptr, stage_size_, PROT_READ | PROT_WRITE, MAP_SHARED, stage_fd_, 0));
    if (stage_map_ == MAP_FAILED) {
        stage_map_ = nullptr;
        std::fprintf(stderr, "TENNIS_RGA setup: mmap staging errno=%d\n", errno);
        return false;
    }

    src_handle_ = importbuffer_fd(stage_fd_, w, h, RK_FORMAT_YUYV_422);
    if (src_handle_ == 0) {
        std::fprintf(stderr, "TENNIS_RGA setup: importbuffer_fd(src) failed\n");
        return false;
    }

    // dst: TRUE zero-copy (Path A) -- make the RGA write straight into the NPU
    // input buffer (no second buffer, no per-frame memcpy). rknn_create_mem backs
    // the NPU input with a dma-buf carrying both an fd and a physical address. Two
    // ways to hand it to the RGA, tried in order:
    //
    //  1. by dma-buf fd (input_mem_->fd) -- IOMMU-safe. This is the Linux path,
    //     where librga may route NV12->RGB888 to an RGA3 core whose IOMMU is on.
    //  2. by raw physical address (input_mem_->phys_addr) -- used only when (1) is
    //     rejected, which is the StarryOS case: our /dev/rga implements only the
    //     RGA2 core, which runs MMU-off and writes a raw phys directly, and the
    //     RK3588 NPU runs IOMMU-disabled so that phys IS the DRAM it reads. This is
    //     SAFE precisely BECAUSE we reach it only after the fd import fails -- i.e.
    //     never on Linux, where a raw phys with no IOVA mapping would fault the
    //     RGA3 IOMMU and HARD-HANG the board. So order matters: fd first, phys
    //     second.
    //
    // Either path keeps dst_is_npu_ = true; coherency is a FROM_DEVICE invalidate
    // after the blit (see prepare_input). (offset != 0 would need an
    // im_handle_param offset; a fresh rknn_create_mem returns offset 0.)
    if (input_mem_->fd > 0 && input_mem_->offset == 0) {
        dst_handle_ = importbuffer_fd(input_mem_->fd, w, h, RK_FORMAT_RGB_888);
        if (dst_handle_ != 0) {
            dst_is_npu_ = true;
            std::fprintf(stderr,
                         "TENNIS_RGA setup: src=%u dst=NPU fd=%d (zero-copy dma-buf)\n",
                         src_handle_, input_mem_->fd);
            rga_ready_ = true;
            return true;
        }
        std::fprintf(stderr,
                     "TENNIS_RGA setup: fd import of NPU buffer rejected; trying raw-phys\n");
    }
    if (input_mem_->phys_addr != 0) {
        // RGA2 MMU-off direct-phys dst -- reached on StarryOS (fd import failed) or
        // when forced. NOT reached on Linux (fd import succeeds), so no IOMMU hang.
        dst_handle_ = importbuffer_physicaladdr(input_mem_->phys_addr, w, h,
                                                RK_FORMAT_RGB_888);
        if (dst_handle_ != 0) {
            dst_is_npu_ = true;
            std::fprintf(stderr,
                         "TENNIS_RGA setup: src=%u dst=NPU phys=0x%llx (zero-copy raw-phys)\n",
                         src_handle_,
                         static_cast<unsigned long long>(input_mem_->phys_addr));
            rga_ready_ = true;
            return true;
        }
        std::fprintf(stderr,
                     "TENNIS_RGA setup: phys import of NPU buffer rejected; "
                     "falling back to dma-heap RGB + memcpy\n");
    }

    // Fallback: a SECOND dma-heap buffer for the RGB888 output (RGA writes it, then
    // prepare_input copies into the NPU io-mem). Used when the NPU buffer can't be
    // imported as an RGA dst.
    rgb_size_ = static_cast<size_t>(w) * static_cast<size_t>(h) * 3; // RGB888
    dma_heap_allocation_data ra;
    std::memset(&ra, 0, sizeof(ra));
    ra.len = rgb_size_;
    ra.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd_, DMA_HEAP_IOCTL_ALLOC, &ra) < 0) {
        std::fprintf(stderr, "TENNIS_RGA setup: DMA_HEAP_ALLOC(rgb) errno=%d\n",
                     errno);
        return false;
    }
    rgb_fd_ = static_cast<int>(ra.fd);
    rgb_map_ = static_cast<unsigned char *>(
        mmap(nullptr, rgb_size_, PROT_READ | PROT_WRITE, MAP_SHARED, rgb_fd_, 0));
    if (rgb_map_ == MAP_FAILED) {
        rgb_map_ = nullptr;
        std::fprintf(stderr, "TENNIS_RGA setup: mmap(rgb) errno=%d\n", errno);
        return false;
    }
    dst_handle_ = importbuffer_fd(rgb_fd_, w, h, RK_FORMAT_RGB_888);
    if (dst_handle_ == 0) {
        std::fprintf(stderr, "TENNIS_RGA setup: importbuffer_fd(dst) failed\n");
        return false;
    }
    std::fprintf(stderr, "TENNIS_RGA setup: src=%u dst=%u (dma-heap RGB + memcpy)\n",
                 src_handle_, dst_handle_);

    rga_ready_ = true;
    return true;
}

void TennisDetector::teardown_preproc() {
    if (src_handle_) {
        releasebuffer_handle(src_handle_);
        src_handle_ = 0;
    }
    if (dst_handle_) {
        releasebuffer_handle(dst_handle_);
        dst_handle_ = 0;
    }
    if (stage_map_) {
        munmap(stage_map_, stage_size_);
        stage_map_ = nullptr;
    }
    if (stage_fd_ >= 0) {
        close(stage_fd_);
        stage_fd_ = -1;
    }
    if (rgb_map_) {
        munmap(rgb_map_, rgb_size_);
        rgb_map_ = nullptr;
    }
    if (rgb_fd_ >= 0) {
        close(rgb_fd_);
        rgb_fd_ = -1;
    }
    if (heap_fd_ >= 0) {
        close(heap_fd_);
        heap_fd_ = -1;
    }
    rga_ready_ = false;
    dst_is_npu_ = false;
    jpu_.deinit();
    jpu_ready_ = false;
    if (input_mem_) {
        rknn_destroy_mem(ctx_.rknn_ctx, input_mem_);
        input_mem_ = nullptr;
    }
    iomem_ = false;
}

namespace {

struct Det {
    float x1, y1, x2, y2, score;
};

double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 +
           static_cast<double>(ts.tv_nsec) / 1.0e6;
}

float iou(const Det &a, const Det &b) {
    const float ix1 = std::max(a.x1, b.x1), iy1 = std::max(a.y1, b.y1);
    const float ix2 = std::min(a.x2, b.x2), iy2 = std::min(a.y2, b.y2);
    const float iw = std::max(0.f, ix2 - ix1), ih = std::max(0.f, iy2 - iy1);
    const float inter = iw * ih;
    const float ua = (a.x2 - a.x1) * (a.y2 - a.y1) +
                     (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
    return ua > 0.f ? inter / ua : 0.f;
}

// RKNN affine dequant: real = (q - zero_point) * scale. Mirrors
// deqnt_affine_to_f32 in the multi-class postprocess (postprocess.cc).
inline float deqnt_affine_to_f32(int8_t q, int32_t zp, float scale) {
    return (static_cast<float>(q) - static_cast<float>(zp)) * scale;
}

} // namespace

// --- Input preparation -------------------------------------------------------

// YUYV camera frame -> RGB888 in the NPU io-mem. RGA fast path with a NEON CPU
// fallback (still into io-mem). Identity letterbox (frame dims == model dims).
int TennisDetector::prepare_input_yuyv(const LatestFrame &f, letterbox_t &lb,
                                       rknn_inference_profile_t * /*prof*/) {
    lb.scale = 1.0f;
    lb.x_pad = 0;
    lb.y_pad = 0;
    const int w = ctx_.model_width, h = ctx_.model_height;
    const int npix = w * h;
    const size_t need = static_cast<size_t>(npix) * 2; // YUYV bytes

    // Drop short/partial frames: on StarryOS the USB ISO path can deliver an
    // incomplete YUYV frame (xhci short-packet), so a fixed-size copy would read
    // past the frame buffer. Mirrors copy_yuyv_as_rgb's size check.
    if (f.data.size() < need) return kShortFrame;

    if (rga_ready_) {
        // RGA YUYV422->RGB888 CSC. Stage the YUYV (one cheap copy out of libuvc's
        // URB buffer), then blit straight into the dst.
        std::memcpy(stage_map_, f.data.data(), need);
        rga_buffer_t src = wrapbuffer_handle(src_handle_, w, h, RK_FORMAT_YUYV_422);
        rga_buffer_t dst = wrapbuffer_handle(dst_handle_, w, h, RK_FORMAT_RGB_888);
        IM_STATUS st = imcvtcolor(src, dst, RK_FORMAT_YUYV_422, RK_FORMAT_RGB_888);
        if (st == IM_STATUS_SUCCESS) {
            if (dst_is_npu_) {
                // Path A zero-copy: RGA wrote the NPU buffer's DRAM directly. The
                // buffer is cacheable, so INVALIDATE its CPU cache lines
                // (FROM_DEVICE) -- this drops any stale/dirty lines so DRAM (RGA's
                // output) is authoritative for the NPU read and the runtime's own
                // pre-run sync can't write back over it. No memcpy.
                rknn_mem_sync(ctx_.rknn_ctx, input_mem_,
                              RKNN_MEMORY_SYNC_FROM_DEVICE);
            } else {
                // Fallback: copy RGA's dma-heap RGB output into the NPU io-mem and
                // flush those CPU-written lines to device.
                std::memcpy(input_mem_->virt_addr, rgb_map_, rgb_size_);
                rknn_mem_sync(ctx_.rknn_ctx, input_mem_,
                              RKNN_MEMORY_SYNC_TO_DEVICE);
            }
            legacy_input_ = false;
            return 0;
        }
        std::fprintf(stderr,
                     "TENNIS_WARN rga imcvtcolor failed (%d); CPU fallback\n", st);
        rga_ready_ = false; // stop trying RGA this run
    }

    // CPU conversion straight into the io-mem buffer. The NPU reads DRAM, so flush
    // the CPU-written cache lines to device before run.
    yuyv_to_rgb888(f.data.data(), static_cast<unsigned char *>(input_mem_->virt_addr),
                   npix);
    rknn_mem_sync(ctx_.rknn_ctx, input_mem_, RKNN_MEMORY_SYNC_TO_DEVICE);
    legacy_input_ = false;
    return 0;
}

// MJPEG camera frame -> JPU hardware decode (NV12 dma-buf) -> RGA NV12->RGB888
// straight into the NPU input. The JPU output is a /dev/dma_heap buffer, so the
// RGA imports it by fd and converts (and resizes if needed) into the same NPU
// dst the YUYV path uses (Path A zero-copy, or the dma-heap-RGB + memcpy
// fallback). Returns 0 on success, -1 to fall back to the CPU JPEG decode.
int TennisDetector::prepare_input_mjpeg(const LatestFrame &f, letterbox_t &lb,
                                        rknn_inference_profile_t * /*prof*/) {
    if (!jpu_ready_ || !rga_ready_) return -1;

    const double t0 = now_ms();
    JpuFrame jf;
    if (!jpu_.decode(f.data.data(), f.data.size(), jf)) return -1;
    const double t_dec = now_ms(); // JPU (MPP) decode wall time

    const int w = ctx_.model_width, h = ctx_.model_height;
    lb.scale = 1.0f;
    lb.x_pad = 0;
    lb.y_pad = 0;

    // Import the JPU NV12 output (a dma-heap fd) as the RGA source for this frame.
    rga_buffer_handle_t src_h =
        importbuffer_fd(jf.fd, jf.hor_stride, jf.ver_stride, RK_FORMAT_YCbCr_420_SP);
    if (src_h == 0) {
        std::fprintf(stderr, "TENNIS_WARN jpu: importbuffer_fd(nv12) failed; CPU fallback\n");
        return -1;
    }
    // The C++ 6-arg overload is wrapbuffer_handle(handle, width, height, FORMAT,
    // wstride, hstride) — the format is the 4th argument, strides are 5th/6th.
    // Passing the strides in the 4th/5th slots made imcheck read hor_stride
    // (640 == 0x280) as the "src format" and reject every blit with
    // "Invalid src format [0x280]" on both Linux and StarryOS.
    rga_buffer_t src = wrapbuffer_handle(src_h, jf.width, jf.height,
                                         RK_FORMAT_YCbCr_420_SP, jf.hor_stride,
                                         jf.ver_stride);
    rga_buffer_t dst = wrapbuffer_handle(dst_handle_, w, h, RK_FORMAT_RGB_888);

    // improcess does the NV12->RGB888 CSC (and any resize) in one RGA pass,
    // straight from the dma-buf src into the dst.
    const bool used_resize = !(jf.width == w && jf.height == h);
    IM_STATUS st;
    {
        im_rect srect = {0, 0, jf.width, jf.height};
        im_rect drect = {0, 0, w, h};
        im_rect prect = {0, 0, 0, 0};
        rga_buffer_t pat;
        std::memset(&pat, 0, sizeof(pat));
        st = improcess(src, dst, pat, srect, drect, prect, IM_SYNC);
    }
    const double t_rga = now_ms(); // RGA NV12->RGB888 (incl import/release) wall time

    releasebuffer_handle(src_h);

    if (st != IM_STATUS_SUCCESS) {
        std::fprintf(stderr, "TENNIS_WARN rga NV12->RGB failed (%d); CPU fallback\n", st);
        return -1;
    }

    if (dst_is_npu_) {
        // Path A zero-copy: RGA wrote the NPU buffer's DRAM. Invalidate CPU lines
        // (FROM_DEVICE) so DRAM is authoritative for the NPU read.
        rknn_mem_sync(ctx_.rknn_ctx, input_mem_, RKNN_MEMORY_SYNC_FROM_DEVICE);
    } else {
        std::memcpy(input_mem_->virt_addr, rgb_map_, rgb_size_);
        rknn_mem_sync(ctx_.rknn_ctx, input_mem_, RKNN_MEMORY_SYNC_TO_DEVICE);
    }
    const double t_sync = now_ms(); // cache sync wall time

    // Comprehensive diagnostics: pinpoint the 25ms StarryOS preprocess gap in ONE
    // board run. Splits the per-frame preprocess into MPP put_packet, the blocking
    // get_frame (MPP pipeline + HW wait), the RGA NV12->RGB CSC, and the cache
    // sync -- tracking avg AND max for each (avg~=max => a fixed wait/timeout;
    // avg<<max => variable) -- plus the RGA path (CSC vs resize+CSC), src/dst dims,
    // JPEG size, and which CPU the decode thread runs on (same-core wakeup is the
    // suspected scheduler cost). (void t0 is the decode-total reference.)
    (void)t0;
    {
        const double put = jf.put_ms;
        const double getf = jf.getframe_ms;
        const double rga = t_rga - t_dec;
        const double sync = t_sync - t_rga;
        static int prep_n = 0;
        static double put_s = 0, getf_s = 0, rga_s = 0, sync_s = 0;
        static double put_mx = 0, getf_mx = 0, rga_mx = 0, sync_mx = 0;
        static double jpeg_kb_s = 0;
        put_s += put;
        getf_s += getf;
        rga_s += rga;
        sync_s += sync;
        if (put > put_mx) put_mx = put;
        if (getf > getf_mx) getf_mx = getf;
        if (rga > rga_mx) rga_mx = rga;
        if (sync > sync_mx) sync_mx = sync;
        jpeg_kb_s += static_cast<double>(f.data.size()) / 1024.0;
        ++prep_n;
        // First few frames verbose (frame 1 includes the one-time info-change +
        // buffer-group setup; later frames are steady state) so warmup never hides
        // in the average, then periodic avg+max so even a short/early-crashing run
        // still emits at least one summary.
        if (prep_n <= 5) {
            std::fprintf(stderr,
                         "TENNIS_JPU_F%d put=%.2f getframe=%.2f rga=%.2f sync=%.2f cpu=%d "
                         "resize=%d src=%dx%d dst=%dx%d jpeg_kb=%.1f\n",
                         prep_n, put, getf, rga, sync, sched_getcpu(), used_resize ? 1 : 0,
                         jf.width, jf.height, w, h,
                         static_cast<double>(f.data.size()) / 1024.0);
        }
        if (prep_n % 30 == 0) {
            std::fprintf(
                stderr,
                "TENNIS_JPU_DIAG n=%d cpu=%d resize=%d src=%dx%d dst=%dx%d "
                "jpeg_kb=%.1f put[avg=%.2f max=%.2f] getframe[avg=%.2f max=%.2f] "
                "rga[avg=%.2f max=%.2f] sync[avg=%.2f max=%.2f]\n",
                prep_n, sched_getcpu(), used_resize ? 1 : 0, jf.width, jf.height, w, h,
                jpeg_kb_s / prep_n, put_s / prep_n, put_mx, getf_s / prep_n, getf_mx,
                rga_s / prep_n, rga_mx, sync_s / prep_n, sync_mx);
        }
    }
    legacy_input_ = false;
    return 0;
}

// Already-decoded RGB888 image -> NPU input. When io-mem is active this writes
// into the NPU buffer via convert_image (RGA-or-CPU, handles resize/letterbox);
// otherwise it falls back to the legacy rknn_inputs_set path.
int TennisDetector::prepare_input_rgb(image_buffer_t *img, letterbox_t &lb,
                                      rknn_inference_profile_t * /*prof*/) {
    if (iomem_) {
        image_buffer_t dst{};
        dst.width = ctx_.model_width;
        dst.height = ctx_.model_height;
        dst.format = IMAGE_FORMAT_RGB888;
        dst.size = get_image_size(&dst);
        dst.fd = input_mem_->fd;
        dst.virt_addr = static_cast<unsigned char *>(input_mem_->virt_addr);
        if (convert_image_with_letterbox(img, &dst, &lb, 114) < 0) return -1;
        legacy_input_ = false;
        return 0;
    }

    // Legacy (no io-mem): identity passthrough when dims already match, else a
    // CPU letterbox into the reusable scratch buffer, then rknn_inputs_set.
    unsigned char *model_input = nullptr;
    const bool dims_match = img->width == ctx_.model_width &&
                            img->height == ctx_.model_height &&
                            img->format == IMAGE_FORMAT_RGB888;
    if (dims_match) {
        lb.scale = 1.0f;
        lb.x_pad = 0;
        lb.y_pad = 0;
        model_input = img->virt_addr;
    } else {
        image_buffer_t dst{};
        dst.width = ctx_.model_width;
        dst.height = ctx_.model_height;
        dst.format = IMAGE_FORMAT_RGB888;
        dst.size = get_image_size(&dst);
        if (scratch_.virt_addr == nullptr || scratch_.size < dst.size) {
            if (scratch_.virt_addr) std::free(scratch_.virt_addr);
            scratch_ = dst;
            scratch_.virt_addr =
                static_cast<unsigned char *>(std::malloc(dst.size));
            if (!scratch_.virt_addr) return -1;
        }
        image_buffer_t use = scratch_;
        use.width = dst.width;
        use.height = dst.height;
        use.format = dst.format;
        if (convert_image_with_letterbox(img, &use, &lb, 114) < 0) return -1;
        model_input = scratch_.virt_addr;
    }

    rknn_input inputs[1];
    std::memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = ctx_.model_width * ctx_.model_height * ctx_.model_channel;
    inputs[0].buf = model_input;
    if (rknn_inputs_set(ctx_.rknn_ctx, ctx_.io_num.n_input, inputs) < 0) return -1;
    legacy_input_ = true;
    return 0;
}

// --- Shared single-class run + decode ---------------------------------------

int TennisDetector::run_decode_single(const letterbox_t &lb, float ow, float oh,
                                      BallObs &out,
                                      rknn_inference_profile_t *prof, double t0) {
    double s = now_ms();
    int rc = rknn_run(ctx_.rknn_ctx, nullptr);
    if (prof) prof->run_ms = now_ms() - s;
    if (rc < 0) return rc;

    const uint32_t n_out = ctx_.io_num.n_output;
    std::vector<rknn_output> outputs(n_out);
    std::memset(outputs.data(), 0, n_out * sizeof(rknn_output));
    for (uint32_t i = 0; i < n_out; ++i) {
        outputs[i].index = i;
        outputs[i].want_float = 0; // keep INT8; dequantize on demand in the decode
    }
    // want_float=0 returns NCHW INT8. Dequantize only score cells that pass the
    // threshold and their 32 or 64 DFL logits.
    s = now_ms();
    rc = rknn_outputs_get(ctx_.rknn_ctx, n_out, outputs.data(), nullptr);
    if (prof) prof->outputs_get_ms = now_ms() - s;
    if (rc < 0) return rc;

    // Split wall-clock run_ms into real NPU compute vs the submit + spin-wait.
    if (prof) {
        rknn_perf_run perf_run;
        std::memset(&perf_run, 0, sizeof(perf_run));
        prof->perf_run_query_ret = rknn_query(
            ctx_.rknn_ctx, RKNN_QUERY_PERF_RUN, &perf_run, sizeof(perf_run));
        if (prof->perf_run_query_ret == RKNN_SUCC)
            prof->rknn_perf_run_ms =
                static_cast<double>(perf_run.run_duration) / 1000.0;
    }

    // Decode each scale: box branch (C==4*reg_max) paired with score C==1.
    std::vector<Det> dets;
    const double dec_start = now_ms();
    for (uint32_t bi = 0; bi < n_out; ++bi) {
        const int box_channels = ctx_.output_attrs[bi].dims[1];
        if (!supported_box_channels(box_channels)) continue;
        const int reg_max = box_channels / 4;
        const int H = ctx_.output_attrs[bi].dims[2];
        const int W = ctx_.output_attrs[bi].dims[3];
        const int stride = H > 0 ? ctx_.model_height / H : 0;
        const int8_t *box_q = static_cast<const int8_t *>(outputs[bi].buf);
        const int32_t box_zp = ctx_.output_attrs[bi].zp;
        const float box_scale = ctx_.output_attrs[bi].scale;
        const int8_t *score_q = nullptr;
        int32_t score_zp = 0;
        float score_scale = 0.f;
        for (uint32_t si = 0; si < n_out; ++si) {
            if (ctx_.output_attrs[si].dims[1] == 1 &&
                ctx_.output_attrs[si].dims[2] == H &&
                ctx_.output_attrs[si].dims[3] == W) {
                score_q = static_cast<const int8_t *>(outputs[si].buf);
                score_zp = ctx_.output_attrs[si].zp;
                score_scale = ctx_.output_attrs[si].scale;
                break;
            }
        }
        if (!score_q || stride <= 0) continue;
        const int hw = H * W;

        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const float sc = deqnt_affine_to_f32(score_q[y * W + x], score_zp,
                                                     score_scale);
                if (sc < cfg_.conf_thresh) continue;
                float dfl[64];
                for (int c = 0; c < box_channels; ++c)
                    dfl[c] = deqnt_affine_to_f32(box_q[c * hw + y * W + x], box_zp,
                                                 box_scale);
                float d[4];
                for (int side = 0; side < 4; ++side) {
                    const float *p = dfl + side * reg_max;
                    float mx = -1e30f;
                    for (int b = 0; b < reg_max; ++b)
                        if (p[b] > mx) mx = p[b];
                    float sum = 0.f, acc = 0.f;
                    for (int b = 0; b < reg_max; ++b) {
                        const float e = std::exp(p[b] - mx);
                        sum += e;
                        acc += e * static_cast<float>(b);
                    }
                    d[side] = sum > 0.f ? acc / sum : 0.f;
                }
                const float cx = static_cast<float>(x) + 0.5f;
                const float cy = static_cast<float>(y) + 0.5f;
                Det det;
                det.x1 = (cx - d[0]) * stride;
                det.y1 = (cy - d[1]) * stride;
                det.x2 = (cx + d[2]) * stride;
                det.y2 = (cy + d[3]) * stride;
                det.score = sc;
                dets.push_back(det);
            }
        }
    }
    if (prof) prof->postprocess_ms = now_ms() - dec_start;

    s = now_ms();
    rknn_outputs_release(ctx_.rknn_ctx, n_out, outputs.data());
    if (prof) prof->outputs_release_ms = now_ms() - s;

    if (!dets.empty()) {
        // Greedy NMS, then keep the largest surviving box in original coords.
        std::sort(dets.begin(), dets.end(),
                  [](const Det &a, const Det &b) { return a.score > b.score; });
        std::vector<char> suppressed(dets.size(), 0);
        const float frame_area =
            static_cast<float>(cfg_.frame_w) * static_cast<float>(cfg_.frame_h);
        float best_area = -1.f;
        for (size_t i = 0; i < dets.size(); ++i) {
            if (suppressed[i]) continue;
            for (size_t j = i + 1; j < dets.size(); ++j) {
                if (!suppressed[j] && iou(dets[i], dets[j]) > cfg_.nms_thresh)
                    suppressed[j] = 1;
            }
            const Det &d = dets[i];
            float x1 = (d.x1 - lb.x_pad) / lb.scale;
            float y1 = (d.y1 - lb.y_pad) / lb.scale;
            float x2 = (d.x2 - lb.x_pad) / lb.scale;
            float y2 = (d.y2 - lb.y_pad) / lb.scale;
            x1 = std::min(std::max(x1, 0.f), ow);
            y1 = std::min(std::max(y1, 0.f), oh);
            x2 = std::min(std::max(x2, 0.f), ow);
            y2 = std::min(std::max(y2, 0.f), oh);
            const float w = x2 - x1, h = y2 - y1;
            const float area = w * h;
            if (area > best_area) {
                best_area = area;
                out.found = true;
                out.w = w;
                out.h = h;
                out.cx = (x1 + x2) / 2.f;
                out.cy = (y1 + y2) / 2.f;
                out.area_ratio = frame_area > 0.f ? area / frame_area : 0.f;
                out.score = d.score;
            }
        }
    }

    if (prof) prof->total_ms = now_ms() - t0;
    return 0;
}

// --- Public detect dispatchers ----------------------------------------------

int TennisDetector::detect(const LatestFrame &frame, BallObs &out,
                           rknn_inference_profile_t *prof) {
    out = BallObs{};
    if (single_class_) return detect_single_class(frame, out, prof);
    // Multi-class fallback: CPU-decode to RGB then the legacy wrapper path.
    if (frame_to_image(frame, &scratch_) != 0) return -1;
    return detect_multi_class(&scratch_, out, prof);
}

int TennisDetector::detect(image_buffer_t *img, BallObs &out,
                           rknn_inference_profile_t *prof) {
    out = BallObs{};
    return single_class_ ? detect_single_class(img, out, prof)
                         : detect_multi_class(img, out, prof);
}

int TennisDetector::detect_single_class(const LatestFrame &frame, BallObs &out,
                                        rknn_inference_profile_t *prof) {
    const double t0 = now_ms();
    if (prof) {
        std::memset(prof, 0, sizeof(*prof));
        prof->perf_run_query_ret = -1;
        prof->rknn_perf_run_ms = -1.0;
    }
    letterbox_t lb{};

    // Fast path: YUYV at the model resolution -> RGA/CPU straight into io-mem.
    if (iomem_ && frame.format == UVC_FRAME_FORMAT_YUYV &&
        frame.width == ctx_.model_width && frame.height == ctx_.model_height) {
        const double s = now_ms();
        const int pr = prepare_input_yuyv(frame, lb, prof);
        if (pr != 0) return pr; // propagate kShortFrame (skip, not an error)
        if (prof) prof->letterbox_ms = now_ms() - s; // "preprocess" == the CSC
        return run_decode_single(lb, static_cast<float>(frame.width),
                                 static_cast<float>(frame.height), out, prof, t0);
    }

    // Fast path: MJPEG -> JPU hardware decode (NV12) -> RGA NV12->RGB888 straight
    // into the NPU io-mem (zero-copy MJPEG->JPU->RGA->NPU).
    if (iomem_ && jpu_ready_ && frame.format == UVC_FRAME_FORMAT_MJPEG) {
        const double s = now_ms();
        const int pr = prepare_input_mjpeg(frame, lb, prof);
        if (pr == 0) {
            if (prof) prof->letterbox_ms = now_ms() - s; // "preprocess" == JPU+CSC
            return run_decode_single(lb, static_cast<float>(ctx_.model_width),
                                     static_cast<float>(ctx_.model_height), out, prof,
                                     t0);
        }
        // pr < 0: JPU/RGA failed -> fall through to the CPU decode path below.
    }

    // Fallback (MJPEG without JPU, or non-matching dims): CPU-decode to RGB.
    if (frame_to_image(frame, &scratch_) != 0) return -1;
    return detect_single_class(&scratch_, out, prof);
}

int TennisDetector::detect_single_class(image_buffer_t *img, BallObs &out,
                                        rknn_inference_profile_t *prof) {
    const double t0 = now_ms();
    if (prof) {
        std::memset(prof, 0, sizeof(*prof));
        prof->perf_run_query_ret = -1;
        prof->rknn_perf_run_ms = -1.0;
    }
    letterbox_t lb{};
    const double s = now_ms();
    if (prepare_input_rgb(img, lb, prof) != 0) return -1;
    if (prof) prof->letterbox_ms = now_ms() - s;
    return run_decode_single(lb, static_cast<float>(img->width),
                             static_cast<float>(img->height), out, prof, t0);
}

// --- Multi-class (COCO fallback) --------------------------------------------
int TennisDetector::detect_multi_class(image_buffer_t *img, BallObs &out,
                                       rknn_inference_profile_t *prof) {
    object_detect_result_list od;
    const int rc = inference_yolov8_model_with_thresholds_profile(
        &ctx_, img, &od, cfg_.conf_thresh, cfg_.nms_thresh, prof);
    if (rc != 0) return rc;

    const float frame_area =
        static_cast<float>(cfg_.frame_w) * static_cast<float>(cfg_.frame_h);
    float best_area = -1.f;
    for (int i = 0; i < od.count; ++i) {
        const object_detect_result &r = od.results[i];
        if (r.cls_id != cfg_.ball_class_id) continue;
        const float w = static_cast<float>(r.box.right - r.box.left);
        const float h = static_cast<float>(r.box.bottom - r.box.top);
        const float area = w * h;
        if (area > best_area) {
            best_area = area;
            out.found = true;
            out.w = w;
            out.h = h;
            out.cx = static_cast<float>(r.box.left + r.box.right) / 2.f;
            out.cy = static_cast<float>(r.box.top + r.box.bottom) / 2.f;
            out.area_ratio = frame_area > 0.f ? area / frame_area : 0.f;
            out.score = r.prop;
        }
    }
    return 0;
}

} // namespace tennis
