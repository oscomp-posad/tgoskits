#include "uvc_capture.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>

#if defined(__linux__)
#include <sched.h>
#endif
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "turbojpeg.h"

const char *uvc_error(UvcApi *api, int err)
{
    if (api->strerror != NULL) {
        const char *msg = api->strerror(err);
        if (msg != NULL) {
            return msg;
        }
    }
    return "unknown libuvc error";
}

static bool load_symbol(void *lib, const char *name, void **out)
{
    *out = dlsym(lib, name);
    if (*out == NULL) {
        printf("dlsym %s failed: %s\n", name, dlerror());
        return false;
    }
    return true;
}

#define LOAD_UVC_SYMBOL(api, field, symbol) \
    load_symbol((api)->lib, symbol, reinterpret_cast<void **>(&((api)->field)))

bool load_uvc_api(UvcApi *api)
{
    const char *candidates[] = {
        "libuvc.so",
        "libuvc.so.0",
        "/usr/local/lib/libuvc.so",
        "/usr/lib/aarch64-linux-gnu/libuvc.so",
        NULL,
    };
    for (int i = 0; candidates[i] != NULL && api->lib == NULL; ++i) {
        api->lib = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
    }
    if (api->lib == NULL) {
        printf("dlopen libuvc.so failed: %s\n", dlerror());
        return false;
    }

    return LOAD_UVC_SYMBOL(api, init, "uvc_init") &&
           LOAD_UVC_SYMBOL(api, exit, "uvc_exit") &&
           LOAD_UVC_SYMBOL(api, get_device_list, "uvc_get_device_list") &&
           LOAD_UVC_SYMBOL(api, free_device_list, "uvc_free_device_list") &&
           LOAD_UVC_SYMBOL(api, ref_device, "uvc_ref_device") &&
           LOAD_UVC_SYMBOL(api, unref_device, "uvc_unref_device") &&
           LOAD_UVC_SYMBOL(api, get_bus_number, "uvc_get_bus_number") &&
           LOAD_UVC_SYMBOL(api, get_device_address, "uvc_get_device_address") &&
           LOAD_UVC_SYMBOL(api, open, "uvc_open") &&
           LOAD_UVC_SYMBOL(api, close, "uvc_close") &&
           LOAD_UVC_SYMBOL(api, get_stream_ctrl_format_size, "uvc_get_stream_ctrl_format_size") &&
           LOAD_UVC_SYMBOL(api, start_streaming, "uvc_start_streaming") &&
           LOAD_UVC_SYMBOL(api, stop_streaming, "uvc_stop_streaming") &&
           LOAD_UVC_SYMBOL(api, strerror, "uvc_strerror");
}

void close_uvc_api(UvcApi *api)
{
    if (api->lib != NULL) {
        dlclose(api->lib);
        api->lib = NULL;
    }
}

static bool looks_like_mjpeg(const unsigned char *data, size_t size)
{
    return size >= 4 && data[0] == 0xff && data[1] == 0xd8 &&
           data[size - 2] == 0xff && data[size - 1] == 0xd9;
}

// Pin the calling (libuvc receive/stash) thread to the cpulist in env
// UVC_CAPTURE_AFFINITY (e.g. "0-3", "4-7", "4"), once. The main thread that
// runs decode + inference is pinned separately via --infer-affinity; this lets
// an affinity sweep place the capture thread independently (e.g. capture on the
// A55 little cores while inference runs on the A76 big cores). No-op if the env
// var is unset or on a non-Linux host.
static void maybe_pin_capture_thread()
{
#if defined(__linux__)
    const char *spec = getenv("UVC_CAPTURE_AFFINITY");
    if (spec == NULL || spec[0] == '\0') {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    bool any = false;
    const char *p = spec;
    while (*p != '\0') {
        char *end = NULL;
        long a = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        long b = a;
        if (*end == '-') {
            const char *q = end + 1;
            char *end2 = NULL;
            long v = strtol(q, &end2, 10);
            if (end2 != q) {
                b = v;
                end = end2;
            }
        }
        for (long c = a; c <= b; ++c) {
            if (c >= 0 && c < CPU_SETSIZE) {
                CPU_SET(static_cast<int>(c), &set);
                any = true;
            }
        }
        p = end;
        while (*p == ',' || *p == ' ') {
            ++p;
        }
    }
    if (!any) {
        return;
    }
    int rc = sched_setaffinity(0, sizeof(set), &set);
    printf("uvc: capture thread affinity '%s' -> %s\n", spec, rc == 0 ? "ok" : "FAILED");
#endif
}

static void frame_callback(UvcFrame *frame, void *ptr)
{
    static bool pinned = false; // single libuvc receive thread, so no race
    if (!pinned) {
        pinned = true;
        maybe_pin_capture_thread();
    }
    if (frame == NULL || ptr == NULL || frame->data == NULL || frame->data_bytes == 0) {
        return;
    }

    const unsigned char *data = reinterpret_cast<const unsigned char *>(frame->data);
    const size_t size = frame->data_bytes;
    int stored_format = frame->frame_format;
    if (looks_like_mjpeg(data, size)) {
        stored_format = UVC_FRAME_FORMAT_MJPEG;
    } else if (frame->frame_format != UVC_FRAME_FORMAT_YUYV) {
        return;
    }

    SharedState *state = reinterpret_cast<SharedState *>(ptr);
    std::lock_guard<std::mutex> guard(state->mutex);
    state->captured++;
    state->bytes += frame->data_bytes;
    if (!state->latest.data.empty() && state->latest.id != state->captured) {
        state->dropped++;
    }
    state->latest.id = state->captured;
    state->latest.sequence = frame->sequence;
    state->latest.width = frame->width;
    state->latest.height = frame->height;
    state->latest.format = stored_format;
    state->latest.data.assign(data, data + size);
}

static int decode_mjpeg(const LatestFrame &frame, image_buffer_t *image)
{
    tjhandle handle = tjInitDecompress();
    if (handle == NULL) {
        printf("tjInitDecompress failed\n");
        return -1;
    }

    int width = 0;
    int height = 0;
    int subsample = 0;
    int colorspace = 0;
    int ret = tjDecompressHeader3(
        handle,
        const_cast<unsigned char *>(frame.data.data()),
        (unsigned long)frame.data.size(),
        &width,
        &height,
        &subsample,
        &colorspace);
    if (ret != 0) {
        printf("tjDecompressHeader3 failed: %s\n", tjGetErrorStr());
        tjDestroy(handle);
        return -1;
    }

    const int size = width * height * 3;
    // Reuse a caller-provided buffer when sufficient (see copy_yuyv_as_rgb);
    // otherwise allocate. Track ownership so the error path only frees a buffer
    // we allocated here, never the caller's persistent one.
    unsigned char *prealloc = image->virt_addr;
    const int prealloc_size = image->size;
    bool owned = false;
    unsigned char *buf;
    if (prealloc != NULL && prealloc_size >= size) {
        buf = prealloc;
    } else {
        buf = reinterpret_cast<unsigned char *>(malloc(size));
        if (buf == NULL) {
            printf("malloc RGB buffer failed: size=%d\n", size);
            tjDestroy(handle);
            return -1;
        }
        owned = true;
    }

    ret = tjDecompress2(
        handle,
        const_cast<unsigned char *>(frame.data.data()),
        (unsigned long)frame.data.size(),
        buf,
        width,
        0,
        height,
        TJPF_RGB,
        0);
    if (ret != 0 && tjGetErrorCode(handle) != 0) {
        printf("tjDecompress2 failed: %s\n", tjGetErrorStr());
        if (owned) free(buf);
        tjDestroy(handle);
        return -1;
    }

    tjDestroy(handle);
    memset(image, 0, sizeof(*image));
    image->width = width;
    image->height = height;
    image->width_stride = width * 3;
    image->height_stride = height;
    image->format = IMAGE_FORMAT_RGB888;
    image->virt_addr = buf;
    image->size = size;
    return 0;
}

#if defined(__ARM_NEON)
// (298*c + chroma) >> 8, then saturate to u8 — the >>8 is floor + the +128
// rounding is folded into `chroma`, so this is byte-identical to the scalar path.
static inline uint8x8_t neon_yuv_chan(int16x8_t c, int32x4_t chroma_lo, int32x4_t chroma_hi)
{
    int32x4_t lo = vmlal_n_s16(chroma_lo, vget_low_s16(c), 298);
    int32x4_t hi = vmlal_n_s16(chroma_hi, vget_high_s16(c), 298);
    return vqmovun_s16(vcombine_s16(vqshrn_n_s32(lo, 8), vqshrn_n_s32(hi, 8)));
}
#endif

// YUYV (4:2:2 packed) -> RGB888, BT.601 full-range. The NEON path does 16 px per
// iteration (vld4 deinterleave -> int32 MACs -> saturating narrow -> vst3) with a
// scalar tail; the scalar fallback is the reference and is byte-identical (verified
// max-diff 0 over random + edge inputs). npix must be even (YUYV pairs).
// Declared in uvc_capture.h so the detector can reuse it as the CPU fallback.
void yuyv_to_rgb888(const unsigned char *src, unsigned char *dst, int npix)
{
    int i = 0;
#if defined(__ARM_NEON)
    const int vec = (npix / 16) * 16; // 16 px = 8 YUYV quads per iteration
    const int16x8_t k16 = vdupq_n_s16(16);
    const int16x8_t k128 = vdupq_n_s16(128);
    const int32x4_t r128 = vdupq_n_s32(128);
    for (; i < vec; i += 16) {
        uint8x8x4_t in = vld4_u8(src); // val0=Y0, val1=U, val2=Y1, val3=V
        int16x8_t y0 = vreinterpretq_s16_u16(vmovl_u8(in.val[0]));
        int16x8_t u = vreinterpretq_s16_u16(vmovl_u8(in.val[1]));
        int16x8_t y1 = vreinterpretq_s16_u16(vmovl_u8(in.val[2]));
        int16x8_t v = vreinterpretq_s16_u16(vmovl_u8(in.val[3]));
        int16x8_t c0 = vsubq_s16(y0, k16);
        int16x8_t c1 = vsubq_s16(y1, k16);
        int16x8_t d = vsubq_s16(u, k128);
        int16x8_t e = vsubq_s16(v, k128);
        int16x4_t dl = vget_low_s16(d), dh = vget_high_s16(d);
        int16x4_t el = vget_low_s16(e), eh = vget_high_s16(e);
        // chroma terms (incl +128 rounding) shared by both luma samples of the pair
        int32x4_t cr_lo = vmlal_n_s16(r128, el, 409), cr_hi = vmlal_n_s16(r128, eh, 409);
        int32x4_t cg_lo = vmlsl_n_s16(vmlsl_n_s16(r128, dl, 100), el, 208);
        int32x4_t cg_hi = vmlsl_n_s16(vmlsl_n_s16(r128, dh, 100), eh, 208);
        int32x4_t cb_lo = vmlal_n_s16(r128, dl, 516), cb_hi = vmlal_n_s16(r128, dh, 516);
        uint8x8_t r0 = neon_yuv_chan(c0, cr_lo, cr_hi);
        uint8x8_t g0 = neon_yuv_chan(c0, cg_lo, cg_hi);
        uint8x8_t b0 = neon_yuv_chan(c0, cb_lo, cb_hi);
        uint8x8_t r1 = neon_yuv_chan(c1, cr_lo, cr_hi);
        uint8x8_t g1 = neon_yuv_chan(c1, cg_lo, cg_hi);
        uint8x8_t b1 = neon_yuv_chan(c1, cb_lo, cb_hi);
        // re-interleave Y0/Y1 pixels back to scan order (px0=Y0,px1=Y1,...)
        uint8x8x2_t zr = vzip_u8(r0, r1), zg = vzip_u8(g0, g1), zb = vzip_u8(b0, b1);
        uint8x16x3_t out;
        out.val[0] = vcombine_u8(zr.val[0], zr.val[1]);
        out.val[1] = vcombine_u8(zg.val[0], zg.val[1]);
        out.val[2] = vcombine_u8(zb.val[0], zb.val[1]);
        vst3q_u8(dst, out);
        src += 32;
        dst += 48;
    }
#endif
    for (; i < npix; i += 2) {
        int y0 = src[0];
        int u = src[1] - 128;
        int y1 = src[2];
        int v = src[3] - 128;
        int c0 = y0 - 16;
        int c1 = y1 - 16;
        int d = u;
        int e = v;
        int r0 = (298 * c0 + 409 * e + 128) >> 8;
        int g0 = (298 * c0 - 100 * d - 208 * e + 128) >> 8;
        int b0 = (298 * c0 + 516 * d + 128) >> 8;
        int r1 = (298 * c1 + 409 * e + 128) >> 8;
        int g1 = (298 * c1 - 100 * d - 208 * e + 128) >> 8;
        int b1 = (298 * c1 + 516 * d + 128) >> 8;
        dst[0] = (unsigned char)std::max(0, std::min(255, r0));
        dst[1] = (unsigned char)std::max(0, std::min(255, g0));
        dst[2] = (unsigned char)std::max(0, std::min(255, b0));
        dst[3] = (unsigned char)std::max(0, std::min(255, r1));
        dst[4] = (unsigned char)std::max(0, std::min(255, g1));
        dst[5] = (unsigned char)std::max(0, std::min(255, b1));
        src += 4;
        dst += 6;
    }
}

static int copy_yuyv_as_rgb(const LatestFrame &frame, image_buffer_t *image)
{
    const int width = frame.width;
    const int height = frame.height;
    const size_t min_size = (size_t)width * (size_t)height * 2;
    if (width <= 0 || height <= 0 || frame.data.size() < min_size) {
        printf("invalid YUYV frame size=%zu expected_at_least=%zu width=%d height=%d\n",
               frame.data.size(),
               min_size,
               width,
               height);
        return -1;
    }

    const int rgb_size = width * height * 3;
    // Reuse a caller-provided buffer of sufficient size (the live consumer keeps one
    // persistent RGB buffer to avoid a malloc/free + page-zero on every frame);
    // otherwise allocate (back-compat for callers that pass an empty image_buffer_t).
    unsigned char *prealloc = image->virt_addr;
    const int prealloc_size = image->size;
    unsigned char *rgb;
    if (prealloc != NULL && prealloc_size >= rgb_size) {
        rgb = prealloc;
    } else {
        rgb = reinterpret_cast<unsigned char *>(malloc(rgb_size));
        if (rgb == NULL) {
            printf("malloc YUYV RGB buffer failed: size=%d\n", rgb_size);
            return -1;
        }
    }

    yuyv_to_rgb888(frame.data.data(), rgb, width * height);

    memset(image, 0, sizeof(*image));
    image->width = width;
    image->height = height;
    image->width_stride = width * 3;
    image->height_stride = height;
    image->format = IMAGE_FORMAT_RGB888;
    image->virt_addr = rgb;
    image->size = rgb_size;
    return 0;
}

int frame_to_image(const LatestFrame &frame, image_buffer_t *image)
{
    if (frame.format == UVC_FRAME_FORMAT_MJPEG) {
        return decode_mjpeg(frame, image);
    }
    if (frame.format == UVC_FRAME_FORMAT_YUYV) {
        return copy_yuyv_as_rgb(frame, image);
    }
    printf("unsupported frame format: %d\n", frame.format);
    return -1;
}

bool snapshot_latest_capture(SharedState *state, LatestFrame *frame)
{
    std::lock_guard<std::mutex> guard(state->mutex);
    if (state->latest.id == 0) {
        return false;
    }
    *frame = state->latest;
    return true;
}

UvcCaptureCounters capture_counters(SharedState *state)
{
    std::lock_guard<std::mutex> guard(state->mutex);
    UvcCaptureCounters counters;
    counters.captured = state->captured;
    counters.bytes = state->bytes;
    counters.dropped = state->dropped;
    return counters;
}

static uvc_device *select_device(UvcApi *api, uvc_context *ctx, int index, const char *log_prefix)
{
    uvc_device **list = NULL;
    int ret = api->get_device_list(ctx, &list);
    if (ret < 0) {
        printf("uvc_get_device_list failed: %s\n", uvc_error(api, ret));
        return NULL;
    }

    uvc_device *selected = NULL;
    for (int i = 0; list != NULL && list[i] != NULL; ++i) {
        printf("%s: device index=%d bus=%u address=%u\n",
               log_prefix,
               i,
               api->get_bus_number(list[i]),
               api->get_device_address(list[i]));
        if (i == index) {
            selected = list[i];
            api->ref_device(selected);
        }
    }
    api->free_device_list(list, 1);
    if (selected == NULL) {
        printf("no UVC device at index %d\n", index);
    }
    return selected;
}

bool uvc_open_and_negotiate(UvcCaptureSession *session,
                            const UvcCaptureOptions *options)
{
    const char *log_prefix = options->log_prefix != NULL ? options->log_prefix : "uvc";
    if (!load_uvc_api(&session->api)) {
        close_uvc_api(&session->api);
        return false;
    }

    int ret = session->api.init(&session->ctx, NULL);
    if (ret < 0) {
        printf("uvc_init failed: %s\n", uvc_error(&session->api, ret));
        close_uvc_api(&session->api);
        return false;
    }

    session->dev = select_device(&session->api, session->ctx, options->device, log_prefix);
    if (session->dev == NULL) {
        stop_uvc_capture(session);
        return false;
    }

    ret = session->api.open(session->dev, &session->devh);
    if (ret < 0) {
        printf("uvc_open failed: %s\n", uvc_error(&session->api, ret));
        stop_uvc_capture(session);
        return false;
    }

    // Prefer MJPEG: the camera frame is decoded on the RK3588 JPU (hardware),
    // then RGA does NV12->RGB888 straight into the NPU input -- the full
    // MJPEG->JPU->RGA->NPU zero-copy pipeline, and MJPEG's ~10x lower USB
    // bandwidth also avoids the YUYV short-frame truncation at 480x640@30. YUYV
    // is the fallback (RGA YUYV422->RGB CSC, still hardware).
    // Force MJPEG (the JPU pipeline needs JPEG frames). The camera advertises MJPG
    // at the requested size, but the first negotiation after a fresh USB
    // enumeration can transiently fail, so retry a few times. Only fall back to
    // YUYV (which DISABLES the JPU decode path) as a loud last resort.
    ret = -1;
    for (int attempt = 0; attempt < 5 && ret < 0; ++attempt) {
        memset(&session->ctrl, 0, sizeof(session->ctrl));
        ret = session->api.get_stream_ctrl_format_size(
            session->devh,
            &session->ctrl,
            UVC_FRAME_FORMAT_MJPEG,
            options->width,
            options->height,
            options->fps);
        if (ret < 0) {
            printf("%s: MJPEG nego attempt %d failed: %s\n", log_prefix, attempt,
                   uvc_error(&session->api, ret));
            usleep(200000); // 200ms before retry
        }
    }
    if (ret < 0) {
        printf("%s: WARNING MJPEG unavailable after retries; falling back to YUYV "
               "(JPU decode path DISABLED)\n",
               log_prefix);
        memset(&session->ctrl, 0, sizeof(session->ctrl));
        ret = session->api.get_stream_ctrl_format_size(
            session->devh,
            &session->ctrl,
            UVC_FRAME_FORMAT_YUYV,
            options->width,
            options->height,
            options->fps);
    }
    if (ret < 0) {
        printf("uvc_get_stream_ctrl_format_size YUYV failed: %s\n", uvc_error(&session->api, ret));
        stop_uvc_capture(session);
        return false;
    }
    printf("%s: ctrl format_index=%u frame_index=%u interval=%u max_frame=%u max_payload=%u iface=%u\n",
           log_prefix,
           session->ctrl.b_format_index,
           session->ctrl.b_frame_index,
           session->ctrl.dw_frame_interval,
           session->ctrl.dw_max_video_frame_size,
           session->ctrl.dw_max_payload_transfer_size,
           session->ctrl.b_interface_number);

    return true;
}

bool uvc_begin_streaming(UvcCaptureSession *session,
                         const UvcCaptureOptions *options)
{
    const char *log_prefix =
        options->log_prefix != NULL ? options->log_prefix : "uvc";
    int ret = session->api.start_streaming(
        session->devh, &session->ctrl, frame_callback, &session->state, 0);
    if (ret < 0) {
        printf("uvc_start_streaming failed: %s\n", uvc_error(&session->api, ret));
        stop_uvc_capture(session);
        return false;
    }
    session->streaming = true;
    printf("%s: streaming started\n", log_prefix);
    return true;
}

bool start_uvc_capture(UvcCaptureSession *session,
                       const UvcCaptureOptions *options)
{
    return uvc_open_and_negotiate(session, options) &&
           uvc_begin_streaming(session, options);
}

void stop_uvc_capture(UvcCaptureSession *session)
{
    if (session->streaming && session->devh != NULL) {
        session->api.stop_streaming(session->devh);
        session->streaming = false;
    }
    if (session->devh != NULL) {
        session->api.close(session->devh);
        session->devh = NULL;
    }
    if (session->dev != NULL) {
        session->api.unref_device(session->dev);
        session->dev = NULL;
    }
    if (session->ctx != NULL) {
        session->api.exit(session->ctx);
        session->ctx = NULL;
    }
    close_uvc_api(&session->api);
}
