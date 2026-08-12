// SPDX-License-Identifier: Apache-2.0
//
// Tennis-ball detector over the (now vendored) RKNN YOLOv8 pipeline.
//
// It reuses the class-agnostic rknn setup (init_yolov8_model / release), then
// decodes the YOLOv8 3-branch head itself. The model class count is detected at
// init from the output tensor shapes:
//   * single-class (a dedicated tennis.rknn, cls tensor [1,1,H,W]) -> a compact
//     built-in DFL decoder (no dependency on the COCO-80 postprocess);
//   * multi-class  (the COCO yolov8.rknn fallback, cls tensor [1,80,H,W]) -> the
//     reused post_process, filtered to cfg.ball_class_id (e.g. 32 = sports ball).
// In both cases NPU all-core mode is enabled and the largest matching box wins.
//
// Zero-copy preprocessing (single-class path): the detector owns its NPU input as
// a dma-buf (rknn_create_mem + rknn_set_io_mem). For a YUYV camera frame the RGA
// 2D accelerator converts YUYV422->RGB888 straight into that NPU buffer (one
// improcess, no CPU color-conversion and no rknn_inputs_set copy). When /dev/rga
// or the dma-buf input is unavailable it falls back to the NEON CPU conversion
// (still into the io-mem buffer), then to the legacy rknn_inputs_set path.
//
// Board-only: requires librknnrt; not part of the host dry-run build.
#pragma once

#include <cstdint>
#include <string>

#include "common.h"      // image_buffer_t
#include "jpu_decoder.h"  // JpuDecoder (hardware MJPEG->NV12)
#include "types.h"
#include "uvc_capture.h"  // LatestFrame
#include "yolov8.h"       // rknn_app_context_t, rknn_inference_profile_t

namespace tennis {

class TennisDetector {
public:
    // detect() return code for an incomplete camera frame (USB ISO short-packet):
    // the frame was dropped, NOT an inference error. Distinct from 0 (ok) and the
    // negative RKNN error codes.
    static constexpr int kShortFrame = 1;

    ~TennisDetector();

    // core_mask: "all"|"auto"|"0"|"1"|"2"|"0_1"|"0_1_2".
    bool init(const char *model_path, const char *label_path, const Config &cfg,
              const std::string &core_mask);
    void deinit();

    // Live path: detect directly from a raw camera frame (YUYV via RGA into the
    // NPU input, or MJPEG/other via CPU decode into the NPU input). Preferred.
    int detect(const LatestFrame &frame, BallObs &out,
               rknn_inference_profile_t *prof);

    // Fixed-image path (validation): detect from an already-decoded RGB888 image.
    int detect(image_buffer_t *img, BallObs &out, rknn_inference_profile_t *prof);

    int model_width() const { return ctx_.model_width; }
    int model_height() const { return ctx_.model_height; }
    bool single_class() const { return single_class_; }
    bool zero_copy() const { return iomem_; }
    bool rga_active() const { return rga_ready_; }

private:
    // --- zero-copy NPU input (single-class only) ---
    bool setup_iomem_input();   // rknn_create_mem + rknn_set_io_mem; sets iomem_
    bool setup_rga();           // dma-heap staging + librga handle import; sets rga_ready_
    void teardown_preproc();

    // Fill the NPU input (io-mem) from a source; set an identity/letterbox map.
    // Returns 0 on success. When io-mem is unavailable, prepare_input_rgb falls
    // back to rknn_inputs_set (legacy_input_ then set true).
    int prepare_input_yuyv(const LatestFrame &f, letterbox_t &lb,
                           rknn_inference_profile_t *prof);
    // MJPEG camera frame -> JPU hardware decode (NV12 dma-buf) -> RGA NV12->RGB888
    // straight into the NPU input (zero-copy MJPEG->JPU->RGA->NPU). Returns 0 on
    // success, -1 to fall back to the CPU JPEG decode path.
    int prepare_input_mjpeg(const LatestFrame &f, letterbox_t &lb,
                            rknn_inference_profile_t *prof);
    int prepare_input_rgb(image_buffer_t *img, letterbox_t &lb,
                          rknn_inference_profile_t *prof);

    // Shared single-class tail: rknn_run + outputs_get + DFL decode + NMS.
    // ow/oh are the ORIGINAL frame dims for un-letterboxing to source coords.
    int run_decode_single(const letterbox_t &lb, float ow, float oh, BallObs &out,
                          rknn_inference_profile_t *prof, double t0);

    int detect_single_class(const LatestFrame &frame, BallObs &out,
                            rknn_inference_profile_t *prof);
    int detect_single_class(image_buffer_t *img, BallObs &out,
                            rknn_inference_profile_t *prof);
    int detect_multi_class(image_buffer_t *img, BallObs &out,
                           rknn_inference_profile_t *prof);

    rknn_app_context_t ctx_{};
    Config cfg_;
    bool inited_ = false;
    bool single_class_ = true;   // detected at init from output channel count
    bool post_inited_ = false;   // COCO post_process initialised (multi-class only)

    // Zero-copy input state (single-class).
    rknn_tensor_mem *input_mem_ = nullptr; // NPU input dma-buf (RGA/CPU target)
    rknn_tensor_attr native_in_attr_{};    // native input attr (deferred set_io_mem)
    bool iomem_ = false;                   // io-mem input active (skip inputs_set)
    bool legacy_input_ = false;            // last prepare used rknn_inputs_set
    int in_wstride_ = 0;                   // native input width stride (pixels)

    // RGA YUYV->RGB fast path.
    bool rga_ready_ = false;
    int heap_fd_ = -1;                     // /dev/dma_heap/system
    int stage_fd_ = -1;                    // YUYV staging dma-buf fd
    unsigned char *stage_map_ = nullptr;   // mmap of stage_fd_
    size_t stage_size_ = 0;
    int rgb_fd_ = -1;                      // RGB888 output dma-buf fd (RGA dst, fallback)
    unsigned char *rgb_map_ = nullptr;     // mmap of rgb_fd_
    size_t rgb_size_ = 0;
    uint32_t src_handle_ = 0;              // librga handle for staging (YUYV src)
    uint32_t dst_handle_ = 0;              // librga handle for RGB dst
    // True zero-copy (Path A): RGA blits straight into the NPU input buffer's
    // physical address (importbuffer_physicaladdr of input_mem_->phys_addr; the NPU
    // has no IOMMU so phys == the DRAM the engine reads), then a FROM_DEVICE cache
    // invalidate makes DRAM authoritative -- no rgb_ staging buffer, no memcpy.
    // false => the rgb_ dma-heap buffer + memcpy fallback (used if the phys import
    // is rejected, e.g. on a board where the NPU buffer isn't RGA-addressable).
    bool dst_is_npu_ = false;

    // Hardware MJPEG decode (JPU via /dev/mpp_service). When ready, an MJPEG camera
    // frame is decoded to NV12 on the JPU and RGA-converted into the NPU input
    // (zero-copy); otherwise the CPU libjpeg-turbo path is used.
    JpuDecoder jpu_;
    bool jpu_ready_ = false;

    // Scratch RGB buffer for MJPEG decode / legacy inputs_set (reused per frame).
    image_buffer_t scratch_{};
};

} // namespace tennis
