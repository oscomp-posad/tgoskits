// SPDX-License-Identifier: Apache-2.0
//
// Tennis-ball detector: a thin wrapper over the reused uvc-rknn RKNN YOLOv8
// pipeline (init_yolov8_model / inference_yolov8_model_with_thresholds_profile /
// post_process from cpp/rknpu2/yolov8.cc + postprocess.cc, Apache-2.0 Rockchip).
// It additionally pins all three RK3588 NPU cores (rknn_set_core_mask) -- which
// the stock image/stream binaries do not -- and reduces the result set to the
// largest box of the configured ball class.
//
// Model note: the reused postprocess decodes the rknn_model_zoo 3-branch YOLOv8
// head. For "works today", point --model at the sibling COCO yolov8.rknn with
// --ball-class 32 (sports ball). A dedicated single-class tennis.rknn must be
// exported in the same 3-branch layout (see model/README.md).
//
// Board-only: requires librknnrt; not part of the host dry-run build.
#pragma once

#include <string>

#include "common.h"  // image_buffer_t
#include "types.h"
#include "yolov8.h"   // rknn_app_context_t, rknn_inference_profile_t

namespace tennis {

class TennisDetector {
public:
    ~TennisDetector();

    // Returns true on success. `core_mask` is "all"|"auto"|"0"|"1"|"2"|"0_1"|"0_1_2".
    bool init(const char *model_path, const char *label_path, const Config &cfg,
              const std::string &core_mask);
    void deinit();

    // Run one inference. Fills `out` with the largest detection whose class id
    // equals cfg.ball_class_id (out.found=false if none). Returns 0 on success,
    // non-zero on an RKNN inference error. `prof` may be null.
    int detect(image_buffer_t *img, BallObs &out, rknn_inference_profile_t *prof);

    int model_width() const { return ctx_.model_width; }
    int model_height() const { return ctx_.model_height; }

private:
    rknn_app_context_t ctx_{};
    Config cfg_;
    bool inited_ = false;
};

} // namespace tennis
