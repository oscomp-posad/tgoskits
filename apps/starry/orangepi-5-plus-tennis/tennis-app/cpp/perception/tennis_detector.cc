// SPDX-License-Identifier: Apache-2.0
#include "tennis_detector.h"

#include "postprocess.h" // object_detect_result_list, init/deinit_post_process
#include "rknn_api.h"    // rknn_set_core_mask, RKNN_NPU_CORE_*

namespace tennis {

static rknn_core_mask parse_core_mask(const std::string &s) {
    if (s == "auto") return RKNN_NPU_CORE_AUTO;
    if (s == "0") return RKNN_NPU_CORE_0;
    if (s == "1") return RKNN_NPU_CORE_1;
    if (s == "2") return RKNN_NPU_CORE_2;
    if (s == "0_1") return RKNN_NPU_CORE_0_1;
    // "0_1_2" / "all" / default: use all three NPU cores.
    return RKNN_NPU_CORE_0_1_2;
}

TennisDetector::~TennisDetector() { deinit(); }

bool TennisDetector::init(const char *model_path, const char *label_path,
                          const Config &cfg, const std::string &core_mask) {
    cfg_ = cfg;
    if (init_yolov8_model(model_path, &ctx_) != 0) return false;
    if (init_post_process(label_path) != 0) {
        release_yolov8_model(&ctx_);
        return false;
    }
    // The stock image/stream binaries never set a core mask (AUTO = one random
    // core); pinning all three is a free latency win on the biggest stage.
    rknn_set_core_mask(ctx_.rknn_ctx, parse_core_mask(core_mask));
    inited_ = true;
    return true;
}

void TennisDetector::deinit() {
    if (!inited_) return;
    release_yolov8_model(&ctx_);
    deinit_post_process();
    inited_ = false;
}

int TennisDetector::detect(image_buffer_t *img, BallObs &out,
                           rknn_inference_profile_t *prof) {
    out = BallObs{};
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
