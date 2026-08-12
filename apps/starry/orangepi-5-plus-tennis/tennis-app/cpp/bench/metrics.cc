// SPDX-License-Identifier: Apache-2.0
#include "metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/resource.h>

namespace tennis {

double percentile_ms(std::vector<double> samples, double p) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    if (samples.size() == 1) return samples[0];
    if (p <= 0.0) return samples.front();
    if (p >= 1.0) return samples.back();
    const double rank = p * static_cast<double>(samples.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(rank));
    const double frac = rank - static_cast<double>(lo);
    return samples[lo] * (1.0 - frac) + samples[lo + 1] * frac;
}

static double avg_ms(const std::vector<double> &v) {
    if (v.empty()) return 0.0;
    double sum = 0.0;
    for (double x : v) sum += x;
    return sum / static_cast<double>(v.size());
}

// avg/p50/p95/p99/p99.9/max for one stage, as a single ` name_*=` run.
static void print_stage(const char *name, const std::vector<double> &v) {
    std::printf(
        " %s_avg=%.3f %s_p50=%.3f %s_p95=%.3f %s_p99=%.3f %s_p999=%.3f "
        "%s_max=%.3f",
        name, avg_ms(v), name, percentile_ms(v, 0.50), name,
        percentile_ms(v, 0.95), name, percentile_ms(v, 0.99), name,
        percentile_ms(v, 0.999), name, percentile_ms(v, 1.0));
}

// Read a `Key:   <kb> kB` value from a /proc file. 0 if not found.
static size_t read_proc_kb(const char *path, const char *key) {
    FILE *f = std::fopen(path, "r");
    if (!f) return 0;
    const size_t klen = std::strlen(key);
    char line[256];
    size_t kb = 0;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, key, klen) == 0) {
            kb = static_cast<size_t>(std::strtoul(line + klen, nullptr, 10));
            break;
        }
    }
    std::fclose(f);
    return kb;
}

size_t read_rss_kb() {
    if (size_t kb = read_proc_kb("/proc/self/status", "VmRSS:")) return kb;
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
#if defined(__APPLE__)
        return static_cast<size_t>(ru.ru_maxrss) / 1024;
#else
        return static_cast<size_t>(ru.ru_maxrss);
#endif
    }
    return 0;
}

void Metrics::reserve(size_t expected_frames) {
    f2d_ms_.reserve(expected_frames);
    f2c_ms_.reserve(expected_frames);
    queue_age_ms_.reserve(expected_frames);
    decode_ms_.reserve(expected_frames);
    letterbox_ms_.reserve(expected_frames);
    inputs_set_ms_.reserve(expected_frames);
    run_ms_.reserve(expected_frames);
    rknn_perf_run_ms_.reserve(expected_frames);
    outputs_get_ms_.reserve(expected_frames);
    postprocess_ms_.reserve(expected_frames);
    control_ms_.reserve(expected_frames);
}

void Metrics::on_processed(double f2d_ms, bool had_ball, bool had_bucket) {
    ++processed_;
    f2d_ms_.push_back(f2d_ms);
    if (had_ball) ++detections_;
    if (had_bucket) ++bucket_detections_;
}

void Metrics::on_stage_timing(const StageTiming &t) {
    queue_age_ms_.push_back(t.queue_age_ms);
    decode_ms_.push_back(t.decode_ms);
    letterbox_ms_.push_back(t.letterbox_ms);
    inputs_set_ms_.push_back(t.inputs_set_ms);
    run_ms_.push_back(t.run_ms);
    rknn_perf_run_ms_.push_back(t.rknn_perf_run_ms);
    outputs_get_ms_.push_back(t.outputs_get_ms);
    postprocess_ms_.push_back(t.postprocess_ms);
    control_ms_.push_back(t.control_ms);
    if (csv_) {
        std::fprintf(csv_,
                     "%llu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%."
                     "4f,%d\n",
                     static_cast<unsigned long long>(t.seq), t.queue_age_ms,
                     t.decode_ms, t.letterbox_ms, t.inputs_set_ms, t.run_ms,
                     t.rknn_perf_run_ms, t.outputs_get_ms, t.postprocess_ms,
                     t.control_ms, t.frame_to_detection_ms,
                     t.frame_to_command_ms, t.detections);
    }
}

void Metrics::record_core(int cpu) {
    if (cpu >= 0 && cpu < kMaxCpus) ++core_hist_[cpu];
}

bool Metrics::open_csv(const char *path) {
    csv_ = std::fopen(path, "w");
    if (!csv_) return false;
    std::fprintf(csv_,
                 "seq,queue_age_ms,decode_ms,letterbox_ms,inputs_set_ms,run_ms,"
                 "rknn_perf_run_ms,outputs_get_ms,postprocess_ms,control_ms,"
                 "frame_to_detection_ms,frame_to_command_ms,detections\n");
    return true;
}

void Metrics::close_csv() {
    if (csv_) {
        std::fclose(csv_);
        csv_ = nullptr;
    }
}

void Metrics::sample_resource(double elapsed_sec) {
    const size_t rss = read_rss_kb();
    const size_t hwm = read_proc_kb("/proc/self/status", "VmHWM:");
    if (rss > rss_hwm_kb_) rss_hwm_kb_ = rss;
    if (hwm > rss_hwm_kb_) rss_hwm_kb_ = hwm;
    std::printf("TENNIS_RES elapsed_sec=%.1f vm_rss_kb=%zu vm_hwm_kb=%zu "
                "mem_total_kb=%zu mem_free_kb=%zu mem_available_kb=%zu\n",
                elapsed_sec, rss, hwm,
                read_proc_kb("/proc/meminfo", "MemTotal:"),
                read_proc_kb("/proc/meminfo", "MemFree:"),
                read_proc_kb("/proc/meminfo", "MemAvailable:"));
}

void Metrics::emit_result(double duration_sec) const {
    std::printf(
        "TENNIS_BENCH_RESULT duration_sec=%.3f captured=%llu processed=%llu "
        "detections=%llu bucket_detections=%llu virtual_motor_commands=%llu "
        "virtual_arm_commands=%llu frame_to_detection_ms_avg=%.3f "
        "frame_to_detection_ms_p50=%.3f frame_to_detection_ms_p95=%.3f "
        "frame_to_detection_ms_p99=%.3f frame_to_detection_ms_p999=%.3f "
        "frame_to_detection_ms_max=%.3f frame_to_command_ms_avg=%.3f "
        "frame_to_command_ms_p50=%.3f frame_to_command_ms_p95=%.3f "
        "frame_to_command_ms_p99=%.3f frame_to_command_ms_p999=%.3f "
        "frame_to_command_ms_max=%.3f decode_errors=%llu inference_errors=%llu "
        "short_frames=%llu memory_rss_kb=%zu memory_hwm_kb=%zu\n",
        duration_sec, static_cast<unsigned long long>(captured_),
        static_cast<unsigned long long>(processed_),
        static_cast<unsigned long long>(detections_),
        static_cast<unsigned long long>(bucket_detections_),
        static_cast<unsigned long long>(motor_commands_),
        static_cast<unsigned long long>(arm_commands_), avg_ms(f2d_ms_),
        percentile_ms(f2d_ms_, 0.50), percentile_ms(f2d_ms_, 0.95),
        percentile_ms(f2d_ms_, 0.99), percentile_ms(f2d_ms_, 0.999),
        percentile_ms(f2d_ms_, 1.0), avg_ms(f2c_ms_),
        percentile_ms(f2c_ms_, 0.50), percentile_ms(f2c_ms_, 0.95),
        percentile_ms(f2c_ms_, 0.99), percentile_ms(f2c_ms_, 0.999),
        percentile_ms(f2c_ms_, 1.0),
        static_cast<unsigned long long>(decode_errors_),
        static_cast<unsigned long long>(inference_errors_),
        static_cast<unsigned long long>(short_frames_), read_rss_kb(),
        rss_hwm_kb_);
}

void Metrics::emit_profile_result() const {
    std::printf("TENNIS_BENCH_PROFILE_RESULT profile_samples=%zu",
                run_ms_.size());
    print_stage("decode_ms", decode_ms_);
    print_stage("letterbox_ms", letterbox_ms_);
    print_stage("inputs_set_ms", inputs_set_ms_);
    print_stage("run_ms", run_ms_);
    print_stage("rknn_perf_run_ms", rknn_perf_run_ms_);
    print_stage("outputs_get_ms", outputs_get_ms_);
    print_stage("postprocess_ms", postprocess_ms_);
    print_stage("control_ms", control_ms_);
    std::printf("\n");
}

void Metrics::emit_pipeline_result() const {
    std::printf("TENNIS_PIPELINE_RESULT samples=%zu", queue_age_ms_.size());
    print_stage("queue_age_ms", queue_age_ms_);
    print_stage("frame_to_detection_ms", f2d_ms_);
    print_stage("frame_to_command_ms", f2c_ms_);
    std::printf("\n");
}

void Metrics::emit_cold_start() const {
    if (!have_cold_) return;
    std::printf("TENNIS_COLD_START capture_init_ms=%.2f model_init_ms=%.2f "
                "first_frame_wait_ms=%.2f first_detection_ms=%.2f "
                "first_command_ms=%.2f time_to_first_command_ms=%.2f\n",
                cold_.capture_init_ms, cold_.model_init_ms,
                cold_.first_frame_wait_ms, cold_.first_detection_ms,
                cold_.first_command_ms, cold_.time_to_first_command_ms);
}

void Metrics::emit_core_hist(const char *affinity) const {
    long total = 0;
    for (long c : core_hist_) total += c;
    std::printf("TENNIS_CORE_HIST requested_affinity=%s samples=%ld",
                affinity ? affinity : "none", total);
    for (int c = 0; c < kMaxCpus; ++c) {
        if (core_hist_[c] > 0) std::printf(" cpu%d=%ld", c, core_hist_[c]);
    }
    std::printf("\n");
}

} // namespace tennis
