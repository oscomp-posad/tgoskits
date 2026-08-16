// STARRY//SIGNAL — 面向 StarryOS / RK3588 的荧光遥测控制台 (phosphor telemetry HUD).
//
// A fullscreen HUD (rendered to the native VOP2 /dev/fb0 via Qt6 linuxfb) that
// fuses live system stats with the tennis-robot app's telemetry. Aesthetic: an
// avionics / oscilloscope readout — scope graticule, corner-bracket framing,
// monospace telemetry, segmented bargraphs, center-zero motor meters, crosshair
// detection reticles, a heading compass, CRT bloom. UI labels are Simplified
// Chinese (Noto Sans CJK SC); numeric data stays in Fira Code.
//
// Data:
//   system  — procfs/sysfs, 1 Hz (/proc/stat per-core, meminfo, loadavg, uptime,
//             thermal_zone0, cpufreq policy0/4/6 for A55 / A76x2)
//   robot   — the app's TENNIS_* stdout telemetry, read from STDIN (non-blocking),
//             so `tennis-app … | dashboard` shows both; `dashboard` alone = system.
//             TENNIS_STATE carries state + ball_area/ball_cx (视觉感知) + the
//             odometry pose odom_valid/x/y/heading/distance (里程计); TENNIS_CMD
//             carries the differential-drive motors + arm action.
//
// Responsive: all geometry scales by min(W/1920, H/1080) so it fits any
// resolution/aspect (16:9, 16:10, 4:3, 21:9) without overflow.
//
// Non-interference (must not perturb the tennis app AT ALL): the dashboard reads
// only telemetry + procfs, caps repaints to ~8 fps, runs SCHED_IDLE pinned to the
// A55 cluster.
//   Recommended:  tennis_app … | QT_QPA_PLATFORM=linuxfb ./dashboard
//   (Do NOT use `--telemetry` to tail a tmpfs file the app is concurrently
//   writing on StarryOS: the concurrent tail can wedge the writer in-kernel —
//   unkillable, reboot-only recovery. Board-proven 2026-08-17; the pipe and
//   the atomically-renamed `--camera` feed file are safe. See DEMO.md.)
// Screenshot (host, offscreen only — never offscreen on StarryOS):
//   QT_QPA_PLATFORM=offscreen ./dashboard --shot out.png [--size 1600x1200]
#define _GNU_SOURCE 1
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QImage>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QTimer>
#include <QSocketNotifier>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QDateTime>
#include <QElapsedTimer>
#include <array>
#include <deque>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif

// ------------------------------ helpers -------------------------------------
static std::string slurp(const char *path) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return {};
    std::string out; char buf[4096]; ssize_t n;
    while ((n = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, size_t(n));
    ::close(fd);
    return out;
}
static std::string kv(const std::string &line, const char *key) {
    std::string k = std::string(key) + "=";
    size_t p = line.find(k);
    if (p == std::string::npos) return {};
    p += k.size();
    size_t e = line.find_first_of(" \t\r\n", p);
    return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}
static double kvd(const std::string &l, const char *k, double d = 0) {
    std::string v = kv(l, k); return v.empty() ? d : std::strtod(v.c_str(), nullptr);
}
static long kvl(const std::string &l, const char *k, long d = 0) {
    std::string v = kv(l, k); return v.empty() ? d : std::strtol(v.c_str(), nullptr, 10);
}
static double nowSec() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

// ------------------------------ theme ---------------------------------------
namespace T {
static const QColor bg0(6, 9, 11);
static const QColor bg1(10, 15, 18);
static const QColor grid(96, 150, 165, 16);
static const QColor gridMaj(96, 150, 165, 30);
static const QColor ink(214, 228, 231);
static const QColor dim(112, 132, 140);
static const QColor faint(38, 50, 56);
static const QColor line(48, 66, 74);
static const QColor green(88, 232, 158);   // ok / signal
static const QColor amber(255, 178, 78);    // robot / active (signature)
static const QColor cyan(84, 200, 230);     // system accent
static const QColor coral(255, 96, 108);    // hot / alert
static const QColor violet(184, 140, 255);  // return / special
static QColor load(double p, QColor cold, QColor mid, QColor hot) {
    if (p >= 82) return hot; if (p >= 50) return mid; return cold;
}
static QColor withA(QColor c, int a) { c.setAlpha(a); return c; }
QString MONO = "Fira Code";                 // Latin numeric / data
QString DISP = "Oswald";                    // Latin display (brand + index)
QString CJK  = "Noto Sans CJK SC";          // 思源黑体 — Chinese labels/headers
} // namespace T

// ------------------------------ system stats --------------------------------
struct CpuT { uint64_t idle = 0, total = 0; };
struct SystemStats {
    static constexpr int N = 8;
    std::array<double, N> cpu{};
    std::array<CpuT, N> prev{};
    bool have = false;
    double aggregate = 0;
    double mem_pct = 0, mem_used_gb = 0, mem_total_gb = 0;
    double temp = 0; bool temp_ok = false;
    std::array<int, 3> freq{}; std::array<bool, 3> freq_ok{};
    double load1 = 0, load5 = 0, load15 = 0; bool load_ok = false;
    uint64_t uptime = 0;
    std::deque<double> hist;  // aggregate CPU history for sparkline

    void sampleCpu() {
        std::string s = slurp("/proc/stat");
        size_t pos = 0; double sum = 0; int cnt = 0;
        while ((pos = s.find("cpu", pos)) != std::string::npos) {
            char c = pos + 3 < s.size() ? s[pos + 3] : 0;
            if (c < '0' || c > '9') { pos += 3; continue; }
            int core = std::atoi(s.c_str() + pos + 3);
            size_t eol = s.find('\n', pos);
            std::string ln = s.substr(pos, eol - pos);
            pos = eol == std::string::npos ? s.size() : eol + 1;
            if (core < 0 || core >= N) continue;
            uint64_t v[8] = {0};
            std::sscanf(ln.c_str() + 4, "%lu %lu %lu %lu %lu %lu %lu %lu",
                        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
            uint64_t idle = v[3] + v[4], tot = 0;
            for (int i = 0; i < 8; i++) tot += v[i];
            if (have) {
                uint64_t dt = tot - prev[core].total, di = idle - prev[core].idle;
                cpu[core] = dt ? 100.0 * double(dt - di) / double(dt) : 0.0;
                sum += cpu[core]; cnt++;
            }
            prev[core] = {idle, tot};
        }
        have = true;
        if (cnt) { aggregate = sum / cnt; hist.push_back(aggregate); while (hist.size() > 120) hist.pop_front(); }
    }
    void sampleRest() {
        std::string mi = slurp("/proc/meminfo");
        auto kb = [&](const char *k) { size_t p = mi.find(k); return p == std::string::npos ? 0.0 : std::strtod(mi.c_str() + p + std::strlen(k), nullptr); };
        double tot = kb("MemTotal:"), av = kb("MemAvailable:");
        mem_total_gb = tot / 1048576.0; mem_used_gb = (tot - av) / 1048576.0;
        mem_pct = tot ? 100.0 * (tot - av) / tot : 0;
        std::string t = slurp("/sys/class/thermal/thermal_zone0/temp");
        temp_ok = !t.empty() && std::strtod(t.c_str(), nullptr) > 0;
        temp = temp_ok ? std::strtod(t.c_str(), nullptr) / 1000.0 : 0;
        const char *pol[3] = {"/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq",
                              "/sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq",
                              "/sys/devices/system/cpu/cpufreq/policy6/scaling_cur_freq"};
        for (int i = 0; i < 3; i++) { std::string f = slurp(pol[i]); freq_ok[i] = !f.empty(); freq[i] = freq_ok[i] ? int(std::strtol(f.c_str(), nullptr, 10) / 1000) : 0; }
        std::string la = slurp("/proc/loadavg");
        load_ok = !la.empty(); if (load_ok) std::sscanf(la.c_str(), "%lf %lf %lf", &load1, &load5, &load15);
        uptime = uint64_t(std::strtod(slurp("/proc/uptime").c_str(), nullptr));
    }
};

// ------------------------------ tennis stats --------------------------------
struct TennisStats {
    bool seen = false;
    std::string state = "STANDBY";
    bool ball = false, bucket = false;
    double frame_age = 0;
    double ball_area = 0, ball_cx = -1, ball_cy = -1;   // perception detail (latest app; cy for the camera overlay)
    double frame_w = 640, frame_h = 480;                // capture dims (for cx/cy normalization)
    std::deque<QPointF> ball_hist;                       // normalized (x,y) ball trail for the motion bridge
    bool odom_valid = false;                            // odometry (latest app)
    double odom_x = 0, odom_y = 0, odom_heading = 0, odom_distance = 0;
    bool anchor_reset = false; double anchor_reset_w = 0;
    std::deque<QPointF> odom_hist;                       // (x,y) trail for the radar
    int mL = 0, mR = 0;
    std::string arm = "—";
    double f2c = 0, ttfi = -1;
    uint64_t rss_kb = 0, frames = 0, detections = 0;
    uint64_t last_f = 0; double last_w = 0, fps = 0;
    bool done = false; double res_fps = 0;
    std::deque<double> f2c_hist;  // frame->command latency history (sparkline)

    void feed(const std::string &l) {
        if (l.rfind("TENNIS_STATE", 0) == 0) {
            seen = true; state = kv(l, "state");
            ball = kvl(l, "detections"); bucket = kvl(l, "bucket_visible");
            ball_area = kvd(l, "ball_area"); ball_cx = kvd(l, "ball_cx", -1); ball_cy = kvd(l, "ball_cy", -1);
            if (ball && ball_cx >= 0) {  // normalized ball trail (frame coords -> 0..1); cy falls back to mid if not emitted
                double nx = ball_cx / frame_w, ny = (ball_cy >= 0 ? ball_cy : frame_h * 0.5) / frame_h;
                ball_hist.push_back(QPointF(nx, ny));
                while (ball_hist.size() > 16) ball_hist.pop_front();
            }
            odom_valid = kvl(l, "odom_valid");
            odom_x = kvd(l, "odom_x"); odom_y = kvd(l, "odom_y");
            odom_heading = kvd(l, "odom_heading"); odom_distance = kvd(l, "odom_distance");
            if (odom_valid) {  // append to trail, thinned so it stays cheap
                if (odom_hist.empty() ||
                    std::hypot(odom_x - odom_hist.back().x(), odom_y - odom_hist.back().y()) > 0.02)
                    odom_hist.push_back(QPointF(odom_x, odom_y));
                while (odom_hist.size() > 240) odom_hist.pop_front();
            }
            frame_age = kvd(l, "frame_age_ms");
            uint64_t f = kvl(l, "frame"); double w = nowSec();
            if (frames && f > last_f && w - last_w > 0.005) {
                double inst = (f - last_f) / (w - last_w);
                if (inst > 120) inst = 120;  // clamp: real capture is <=60 fps
                fps = fps == 0 ? inst : fps * 0.8 + inst * 0.2;
            }
            last_f = f; last_w = w; frames++;
        } else if (l.rfind("TENNIS_CMD", 0) == 0) {
            seen = true; mL = kvl(l, "motor_left"); mR = kvl(l, "motor_right");
            arm = kv(l, "arm_action"); f2c = kvd(l, "frame_to_command_ms");
            f2c_hist.push_back(f2c); while (f2c_hist.size() > 120) f2c_hist.pop_front();
        } else if (l.rfind("TENNIS_RES", 0) == 0) { seen = true; rss_kb = kvl(l, "vm_rss_kb"); }
        else if (l.rfind("TENNIS_FIRST_INFERENCE", 0) == 0) { seen = true; ttfi = kvd(l, "ms_since_proc_start"); }
        else if (l.rfind("TENNIS_BENCH_RESULT", 0) == 0) {
            seen = true; done = true; double d = kvd(l, "duration_sec"), pr = kvd(l, "processed");
            res_fps = d > 0 ? pr / d : 0; detections = kvl(l, "detections");
        }
    }
};

static QColor stateColor(const std::string &s) {
    if (s.find("CHASE") != std::string::npos) return T::cyan;
    if (s.find("GRAB") != std::string::npos) return T::amber;
    if (s.find("RETURN") != std::string::npos) return T::violet;
    if (s.find("FIND") != std::string::npos) return QColor(120, 210, 200);
    if (s.find("APPROACH") != std::string::npos) return T::green;
    if (s.find("DEPOSIT") != std::string::npos) return T::green;
    return T::dim;
}

// State machine → Chinese (substring match mirrors stateColor; covers brake sub-phases).
static QString stateCN(const std::string &s) {
    if (s.find("BRAKE") != std::string::npos || s.find("Brake") != std::string::npos) return "制动";
    if (s.find("CHASE") != std::string::npos) return "追球";
    if (s.find("GRAB") != std::string::npos) return "抓取";
    if (s.find("RETURN") != std::string::npos) return "返回球桶";
    if (s.find("FIND") != std::string::npos) return "寻找球桶";
    if (s.find("APPROACH") != std::string::npos) return "接近球桶";
    if (s.find("DEPOSIT") != std::string::npos) return "投放";
    if (s.find("SEARCH") != std::string::npos) return "搜索";
    if (s.find("STANDBY") != std::string::npos || s.find("IDLE") != std::string::npos) return "待机";
    return QString::fromStdString(s);
}

// Arm action → Chinese.
static QString armCN(const std::string &a) {
    if (a == "grab") return "抓取";
    if (a == "release") return "释放";
    if (a == "ready") return "就绪";
    if (a.empty() || a == "None" || a == "none" || a == "—") return "无";
    return QString::fromStdString(a);
}

// ------------------------------ camera feed ---------------------------------
// Live camera frames the tennis app publishes to a tmpfs file (atomic rename), at
// low fps. Format: 8-byte header 'C','F', u16 seq, u16 w, u16 h (little-endian),
// then w*h*4 RGBA8888. We keep the current + previous frame so a new arrival can
// crossfade in — the low-fps feed then reads as fluid next to the realtime
// detection overlay (which updates at telemetry rate). Zero cost to the app
// beyond the small downscaled frame it already chose to publish.
struct CameraFeed {
    std::string path;
    QImage cur, prev;
    double curArrival = 0;
    int seq = -1;
    bool have() const { return !cur.isNull(); }
    void set(const QImage &img) { prev = cur; cur = img; curArrival = nowSec(); }
    void poll() {
        if (path.empty()) return;
        std::string d = slurp(path.c_str());
        if (d.size() < 8) return;
        const unsigned char *p = (const unsigned char *)d.data();
        if (p[0] != 'C' || p[1] != 'F') return;
        int s = p[2] | (p[3] << 8), w = p[4] | (p[5] << 8), h = p[6] | (p[7] << 8);
        if (s == seq || w <= 0 || h <= 0 || d.size() < size_t(8) + size_t(w) * h * 4) return;
        QImage img((const uchar *)(p + 8), w, h, QImage::Format_RGBA8888);
        set(img.copy());  // detach from the slurped buffer
        seq = s;
    }
    double fade() const { double dt = nowSec() - curArrival; return std::min(1.0, dt / 0.12); }  // 120 ms crossfade
};

// ------------------------------ dashboard -----------------------------------
class Dashboard : public QWidget {
public:
    explicit Dashboard(bool demo = false, const QString &telePath = QString(), int renderFps = 8)
        : renderFps_(renderFps) {
        paintDbg_ = (::getenv("STARRY_PAINT_DEBUG") != nullptr);
        // Our paintEvent fills every pixel (background() first), so claim opaque
        // painting: Qt skips its pre-paint erase and flushes the whole widget — on
        // StarryOS linuxfb the erase-then-partial-flush left only the background.
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        sys.sampleCpu(); sys.sampleRest();
        phase.start();
        if (demo) injectDemo();
        if (!demo) {
            auto *dt = new QTimer(this);  // system stats @ 1 Hz
            connect(dt, &QTimer::timeout, this, [this] { sys.sampleCpu(); sys.sampleRest(); dirty_ = true; });
            dt->start(1000);
            if (!telePath.isEmpty()) {
                // Tail a telemetry file (run the app as `tennis_app … > /tmp/tt.log`).
                // Fully decoupled: the app writes to (tmpfs) page cache and never
                // waits on us — unlike a pipe, whose full buffer would BLOCK the
                // app's stdout write and stall its control loop.
                fd_ = ::open(telePath.toUtf8().constData(), O_RDONLY | O_NONBLOCK);
                if (fd_ >= 0) ::lseek(fd_, 0, SEEK_END);
                auto *pt = new QTimer(this);
                connect(pt, &QTimer::timeout, this, [this] { drain(); });
                pt->start(40);
            } else {
                // stdin pipe: enlarge the pipe buffer so the app keeps a big write
                // cushion and (with our prompt draining) never blocks on stdout.
                fd_ = 0;
                ::fcntl(0, F_SETFL, ::fcntl(0, F_GETFL) | O_NONBLOCK);
                (void)::fcntl(0, F_SETPIPE_SZ, 1 << 20);
                auto *sn = new QSocketNotifier(0, QSocketNotifier::Read, this);
                connect(sn, &QSocketNotifier::activated, this, [this] { drain(); });
            }
        }
        // Repaint at a fixed low rate (renderFps_, default 8) — unconditional, like
        // the v1 board-validated build. Dirty-gating the repaint confused the
        // StarryOS linuxfb flush (only the background reached /dev/fb0), and a fixed
        // 8 fps is already low enough for the non-interference budget.
        (void)dirty_; (void)lastPaint_;
        auto *rt = new QTimer(this);
        connect(rt, &QTimer::timeout, this, [this] { cam.poll(); update(); });
        rt->start(std::max(30, 1000 / std::max(1, renderFps_)));
    }
    void setCameraPath(const std::string &p) { cam.path = p; }
    // Timer-independent tick for the --directfb manual present loop (StarryOS Qt
    // timers don't reliably fire under linuxfb): poll the camera file + drain the
    // telemetry tail, exactly what the rt/pt QTimers would have done.
    void pump() { cam.poll(); drain(); }
    void injectDemo() {
        double v[8] = {22, 14, 9, 31, 88, 74, 61, 45};
        for (int i = 0; i < 8; i++) sys.cpu[i] = v[i];
        sys.aggregate = 43; sys.mem_pct = 38; sys.mem_used_gb = 3.0; sys.mem_total_gb = 7.7;
        sys.temp = 58.4; sys.temp_ok = true; sys.freq = {1800, 2256, 2256}; sys.freq_ok = {true, true, true};
        sys.load1 = 2.31; sys.load5 = 1.9; sys.load15 = 1.4; sys.load_ok = true;
        sys.uptime = 4293; sys.have = true;
        for (int i = 0; i < 120; i++) sys.hist.push_back(38 + 30 * std::sin(i * 0.21) + 12 * std::sin(i * 0.63) + (i % 7) * 1.5);
        tn.seen = true; tn.state = "CHASE_BALL"; tn.ball = true; tn.bucket = false; tn.frame_age = 11.4;
        tn.mL = 42; tn.mR = -42; tn.arm = "None"; tn.f2c = 23.7; tn.ttfi = 8420; tn.rss_kb = 41216;
        tn.frames = 1873; tn.detections = 512; tn.fps = 29.6;
        tn.ball_area = 0.038; tn.ball_cx = 366; tn.ball_cy = 300;
        for (int i = 0; i <= 8; i++) { double t = i / 8.0; tn.ball_hist.push_back(QPointF(0.50 + 0.072 * t, 0.58 + 0.045 * t)); }  // trail tail(stale)→head(now)
        // synthetic camera frame for the demo: a court scene + the tennis ball
        QImage frame(320, 240, QImage::Format_RGBA8888);
        { QPainter cp(&frame); cp.setRenderHint(QPainter::Antialiasing);
          QLinearGradient g(0, 0, 0, 240); g.setColorAt(0, QColor(46, 78, 60)); g.setColorAt(1, QColor(16, 30, 25)); cp.fillRect(frame.rect(), g);
          cp.setPen(QPen(QColor(130, 190, 160, 80), 1));
          for (int i = 1; i < 6; i++) { int y = 240 * i / 6; cp.drawLine(0, y, 320, y); }
          for (int i = 0; i <= 8; i++) { double x = 320.0 * i / 8; cp.drawLine(160 + (x - 160) * 0.35, 70, x, 240); }
          double bx = 0.50 * 320, by = 0.58 * 240;  // camera ball = trail tail (slightly behind the live overlay)
          QRadialGradient bg(bx, by, 15); bg.setColorAt(0, QColor(225, 245, 95)); bg.setColorAt(1, QColor(150, 180, 40));
          cp.setBrush(bg); cp.setPen(Qt::NoPen); cp.drawEllipse(QPointF(bx, by), 15, 15); }
        cam.cur = frame;
        tn.odom_valid = true; tn.odom_x = 1.24; tn.odom_y = -0.42; tn.odom_heading = -0.62; tn.odom_distance = 1.31;
        for (int i = 0; i <= 60; i++) {  // curved path from the anchor out to the robot
            double t = i / 60.0, a = t * 1.9;
            tn.odom_hist.push_back(QPointF(1.24 * t + 0.15 * std::sin(a * 3), -0.42 * t + 0.18 * std::sin(a * 2)));
        }
        for (int i = 0; i < 120; i++) tn.f2c_hist.push_back(22 + 9 * std::sin(i * 0.28) + 5 * std::sin(i * 0.91) + (i % 5) * 0.8);
    }

protected:
    void dbg(const char *s) const { if (dbgActive_) { std::fprintf(stderr, "DBG %s\n", s); std::fflush(stderr); } }
    void paintEvent(QPaintEvent *) override {
        dbgActive_ = paintDbg_ && paintN_++ < 2;
        dbg("start");
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const int W = width(), H = height();
        background(p, W, H); dbg("bg");
        int m = std::max(16, W / 84);
        int hh = std::max(52, H / 16);
        topbar(p, m, m, W - 2 * m, hh); dbg("topbar");
        int gap = std::max(26, W / 50);              // generous gutter between the three panels
        int top = m + hh + int(gap * 0.9), bh = H - top - m;
        // three columns: 系统 (system) | 机器人视角 (vision) | 场地雷达 (nav)
        int avail = W - 2 * m - 2 * gap;
        int wA = int(avail * 0.30), wB = int(avail * 0.40), wC = avail - wA - wB;
        systemPanel(p, m, top, wA, bh); dbg("sys");
        visionCol(p, m + wA + gap, top, wB, bh); dbg("vision");
        navCol(p, m + wA + gap + wB + gap, top, wC, bh); dbg("nav");
        dbgActive_ = false;
    }

private:
    SystemStats sys; TennisStats tn; std::string buf; CameraFeed cam;
    QElapsedTimer phase;
    int fd_ = 0, renderFps_ = 8; bool dirty_ = true; qint64 lastPaint_ = 0;
    bool paintDbg_ = false; mutable bool dbgActive_ = false; int paintN_ = 0;  // STARRY_PAINT_DEBUG hang-locator
    // Responsive scale: driven by the tighter of the two axes so the HUD fits
    // any resolution/aspect (16:9, 16:10, 4:3, 21:9) without text overflow. On a
    // 16:9 panel W/1920 == H/1080, so this matches the 1080p reference exactly.
    double scale() const { return std::min(width() / 1920.0, height() / 1080.0); }
    double fs(double b) const { return b * scale(); }
    QFont mono(double pt, int weight = QFont::Medium) const { QFont f(T::MONO); f.setPointSizeF(fs(pt)); f.setWeight(QFont::Weight(weight)); return f; }
    QFont disp(double pt, double spacing = 3.0) const { QFont f(T::DISP); f.setPointSizeF(fs(pt)); f.setBold(true); f.setLetterSpacing(QFont::AbsoluteSpacing, fs(spacing)); f.setCapitalization(QFont::AllUppercase); return f; }
    // Chinese label/header font (思源黑体). Latin glyphs in the same string render
    // from this face too, so mixed "4× A55 · 能效核" stays visually consistent.
    QFont cjk(double pt, int weight = QFont::Medium, double spacing = 1.0) const { QFont f(T::CJK); f.setPointSizeF(fs(pt)); f.setWeight(QFont::Weight(weight)); if (spacing) f.setLetterSpacing(QFont::AbsoluteSpacing, fs(spacing)); return f; }

    void drain() {
        char b[16384]; ssize_t n;
        while ((n = ::read(fd_, b, sizeof b)) > 0) buf.append(b, size_t(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) { tn.feed(buf.substr(0, nl)); buf.erase(0, nl + 1); }
        if (buf.size() > (1u << 20)) buf.clear();  // guard against an unbounded partial line
        dirty_ = true;  // repaint happens on the throttled render tick, not here
    }

    // ---- atmosphere ----
    void background(QPainter &p, int W, int H) {
        QLinearGradient g(0, 0, 0, H); g.setColorAt(0, T::bg1); g.setColorAt(1, T::bg0);
        p.fillRect(0, 0, W, H, g);
        // graticule
        int step = std::max(28, H / 34);
        p.setPen(QPen(T::grid, 1));
        for (int x = 0; x <= W; x += step) p.drawLine(x, 0, x, H);
        for (int y = 0; y <= H; y += step) p.drawLine(0, y, W, y);
        p.setPen(QPen(T::gridMaj, 1));
        for (int x = 0; x <= W; x += step * 5) p.drawLine(x, 0, x, H);
        for (int y = 0; y <= H; y += step * 5) p.drawLine(0, y, W, y);
        // vignette
        QRadialGradient v(W / 2.0, H / 2.0, W * 0.62);
        v.setColorAt(0, QColor(0, 0, 0, 0)); v.setColorAt(1, QColor(0, 0, 0, 150));
        p.fillRect(0, 0, W, H, v);
        // scanlines
        p.setPen(QPen(QColor(0, 0, 0, 26), 1));
        for (int y = 0; y < H; y += 3) p.drawLine(0, y, W, y);
    }

    // ---- HUD panel frame: corner brackets + index label ----
    void frame(QPainter &p, QRect r, const QString &idx, const QString &title, QColor accent) {
        int b = int(fs(22)), t = std::max(2, int(fs(2)));
        // panel surface (subtle, so the graticule reads faintly through)
        p.setPen(Qt::NoPen); p.setBrush(T::withA(T::bg0, 140)); p.drawRect(r);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(T::withA(accent, 220), t, Qt::SolidLine, Qt::FlatCap));
        // four corners
        p.drawLine(r.left(), r.top(), r.left() + b, r.top());       p.drawLine(r.left(), r.top(), r.left(), r.top() + b);
        p.drawLine(r.right(), r.top(), r.right() - b, r.top());     p.drawLine(r.right(), r.top(), r.right(), r.top() + b);
        p.drawLine(r.left(), r.bottom(), r.left() + b, r.bottom()); p.drawLine(r.left(), r.bottom(), r.left(), r.bottom() - b);
        p.drawLine(r.right(), r.bottom(), r.right() - b, r.bottom()); p.drawLine(r.right(), r.bottom(), r.right(), r.bottom() - b);
        // faint full edge
        p.setPen(QPen(T::withA(accent, 40), 1)); p.drawRect(r);
        // header: a small accent tab + the title (no index number)
        (void)idx;
        int y = r.top() + int(fs(32));
        double tabX = r.left() + fs(22);
        p.setBrush(accent); p.setPen(Qt::NoPen);
        p.drawRect(QRectF(tabX, y - fs(13), fs(4), fs(15)));  // accent tab replaces the index
        p.setFont(cjk(15, QFont::Bold, 3)); p.setPen(T::ink);
        p.drawText(int(tabX + fs(16)), y, title);
        // right-side tick ruler
        p.setPen(QPen(T::line, 1));
        for (int i = 0; i < 6; i++) { int tx = r.right() - int(fs(20)) - i * int(fs(12)); p.drawLine(tx, r.top() + int(fs(16)), tx, r.top() + int(fs(16)) + (i % 2 ? int(fs(6)) : int(fs(10)))); }
    }

    // ---- segmented bargraph ----
    void seg(QPainter &p, QRectF r, double frac, QColor c, int n = 24) {
        frac = std::max(0.0, std::min(1.0, frac));
        double gap = r.width() * 0.010, sw = (r.width() - gap * (n - 1)) / n;
        int lit = int(std::round(frac * n));
        for (int i = 0; i < n; i++) {
            QRectF s(r.left() + i * (sw + gap), r.top(), sw, r.height());
            if (i < lit) {
                QColor cc = c;
                if (i > n * 0.82) cc = T::coral; else if (i > n * 0.62) cc = T::amber;
                p.fillRect(s, cc);
                p.fillRect(s.adjusted(0, 0, 0, -s.height() * 0.55), T::withA(Qt::white, 30));
            } else p.fillRect(s, T::withA(c, 22));
        }
    }

    // ---- center-zero bidirectional meter (motors) ----
    void motor(QPainter &p, QRectF r, int val) {
        int n = 20; double gap = r.width() * 0.008, sw = (r.width() - gap * (n - 1)) / n;
        double cx = r.center().x();
        int lit = int(std::round(std::min(100, std::abs(val)) / 100.0 * (n / 2)));
        QColor c = val >= 0 ? T::green : T::coral;
        for (int i = 0; i < n; i++) {
            QRectF s(r.left() + i * (sw + gap), r.top(), sw, r.height());
            bool right = s.center().x() > cx;
            int dist = int(std::abs(s.center().x() - cx) / (sw + gap));
            bool on = (val >= 0 && right && dist <= lit) || (val < 0 && !right && dist <= lit);
            p.fillRect(s, on ? c : T::withA(T::line, 120));
            if (on) p.fillRect(s.adjusted(0, 0, 0, -s.height() * 0.55), T::withA(Qt::white, 30));
        }
        p.setPen(QPen(T::withA(Qt::white, 90), std::max(1, int(fs(1))))); p.drawLine(QPointF(cx, r.top() - fs(3)), QPointF(cx, r.bottom() + fs(3)));
    }

    // ---- reticle (detection indicator) ----
    void reticle(QPainter &p, double cx, double cy, double rad, bool on, QColor c, const QString &label) {
        QColor col = on ? c : T::withA(T::dim, 160);
        if (on) { // bloom
            for (int k = 3; k >= 1; k--) { p.setPen(QPen(T::withA(c, 22 * k), fs(1.2))); p.setBrush(Qt::NoBrush); p.drawEllipse(QPointF(cx, cy), rad + k * fs(3), rad + k * fs(3)); }
        }
        p.setPen(QPen(col, std::max(1, int(fs(1.6))))); p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(cx, cy), rad, rad);
        double t = rad * 0.55;
        p.drawLine(QPointF(cx - rad - fs(3), cy), QPointF(cx - t, cy)); p.drawLine(QPointF(cx + t, cy), QPointF(cx + rad + fs(3), cy));
        p.drawLine(QPointF(cx, cy - rad - fs(3)), QPointF(cx, cy - t)); p.drawLine(QPointF(cx, cy + t), QPointF(cx, cy + rad + fs(3)));
        if (on) { p.setBrush(c); p.setPen(Qt::NoPen); p.drawEllipse(QPointF(cx, cy), rad * 0.28, rad * 0.28); }
        p.setFont(cjk(12, on ? QFont::Bold : QFont::Normal)); p.setPen(on ? T::ink : T::dim);
        p.drawText(QPointF(cx + rad + fs(16), cy + fs(5)), label);
    }

    void glow(QPainter &p, const QFont &f, QColor c, double x, double y, const QString &s, int a = 60) {
        p.setFont(f);
        p.setPen(T::withA(c, a)); p.drawText(QPointF(x, y), s);
        p.setPen(c); p.drawText(QPointF(x, y), s);
    }

    void topbar(QPainter &p, int x, int y, int w, int h) {
        QRect r(x, y, w, h);
        p.setPen(QPen(T::line, 1)); p.setBrush(T::withA(T::bg1, 200)); p.drawRect(r); dbg("tb:rect");
        p.setFont(disp(24, 5)); glow(p, disp(24, 5), T::amber, x + fs(20), y + h * 0.66, "STARRY//SIGNAL", 70); dbg("tb:latin");
        int wm = p.boundingRect(0, 0, w, h, 0, "STARRY//SIGNAL").width();
        double sx = x + fs(20) + wm + fs(34);
        p.setPen(QPen(T::withA(T::amber, 90), 1)); p.drawLine(QPointF(sx - fs(18), y + h * 0.30), QPointF(sx - fs(18), y + h * 0.72));
        p.setFont(cjk(11)); p.setPen(T::dim);
        p.drawText(QPointF(sx, y + h * 0.64), "内核实时遥测 · RK3588 · 香橙派 5 Plus"); dbg("tb:cjk");
        // right cluster, laid out right-to-left: [clock]  [uptime]
        double yb = y + h * 0.62;
        double cur = x + w - fs(24);
        QString clk = QDateTime::currentDateTime().toString("HH:mm:ss");
        p.setFont(mono(17, QFont::Bold)); int cw = p.boundingRect(0, 0, w, h, 0, clk).width();
        cur -= cw; p.setPen(T::ink); p.drawText(QPointF(cur, yb), clk); cur -= fs(34);
        QString up = QString::asprintf("运行 %llu:%02llu:%02llu", (unsigned long long)(sys.uptime / 3600), (unsigned long long)((sys.uptime / 60) % 60), (unsigned long long)(sys.uptime % 60));
        p.setFont(cjk(11)); int uw = p.boundingRect(0, 0, w, h, 0, up).width();
        cur -= uw; p.setPen(T::dim); p.drawText(QPointF(cur, yb), up);
    }

    void systemPanel(QPainter &p, int x, int y, int w, int h) {
        QRect R(x, y, w, h);
        frame(p, R, "01", "系统", T::cyan);
        int ix = x + int(fs(26)), iw = w - int(fs(52));
        int cy = y + int(fs(74));
        double rh = h * 0.042, rgap = fs(6.5), csep = fs(16);
        for (int c = 0; c < 8; c++) {
            double ry = cy + c * (rh + rgap) + (c >= 4 ? csep : 0);
            bool little = c < 4; QColor acc = little ? T::green : T::cyan;
            if (c == 0 || c == 4) {  // cluster tag on the left
                p.setFont(cjk(9, QFont::Bold)); p.setPen(T::withA(acc, 200));
                p.drawText(QPointF(ix, ry - fs(6)), little ? "4× A55 · 能效核" : "4× A76 · 性能核");
            }
            p.setFont(mono(11, QFont::Bold)); p.setPen(acc);
            p.drawText(QPointF(ix, ry + rh * 0.72), QString::asprintf("C%d", c));
            double bx = ix + fs(40), bw = iw - fs(40) - fs(64);
            seg(p, QRectF(bx, ry, bw, rh), sys.cpu[c] / 100.0, acc, 26);
            p.setFont(mono(13, QFont::Bold)); p.setPen(T::load(sys.cpu[c], T::ink, T::amber, T::coral));
            p.drawText(QRectF(ix + iw - fs(60), ry, fs(60), rh), Qt::AlignRight | Qt::AlignVCenter, QString::asprintf("%3.0f%%", sys.cpu[c]));
        }
        cy += 8 * (rh + rgap) + csep + fs(26);
        // aggregate sparkline
        p.setFont(cjk(12, QFont::Bold, 2)); p.setPen(T::dim); p.drawText(ix, cy, "总负载");
        p.setFont(mono(13, QFont::Bold)); p.setPen(T::cyan);
        p.drawText(QRectF(ix, cy - fs(12), iw, fs(16)), Qt::AlignRight, QString::asprintf("%.0f%%", sys.aggregate));
        cy += fs(8);
        QRectF spark(ix, cy, iw, h * 0.09);
        sparkline(p, spark, sys.hist, T::cyan);
        cy += spark.height() + fs(30);
        // memory
        p.setFont(cjk(12, QFont::Bold, 2)); p.setPen(T::dim); p.drawText(ix, cy, "内存");
        cy += fs(10);
        seg(p, QRectF(ix, cy, iw, h * 0.036), sys.mem_pct / 100.0, T::green, 40);
        cy += h * 0.036 + fs(20);
        p.setFont(mono(12)); p.setPen(T::ink);
        p.drawText(QPointF(ix, cy), QString::asprintf("%.1f / %.1f GiB", sys.mem_used_gb, sys.mem_total_gb));
        cy += fs(30);
        // stat tiles: temp / load / freqs
        double tgap = fs(14); double tw = (iw - tgap * 2) / 3, th = h * 0.115;
        stat(p, ix, cy, tw, th, "SoC 温度", sys.temp_ok ? QString::asprintf("%.1f", sys.temp) : "—", sys.temp_ok ? "°C" : "", !sys.temp_ok ? T::dim : sys.temp > 80 ? T::coral : sys.temp > 65 ? T::amber : T::green);
        stat(p, ix + tw + tgap, cy, tw, th, "负载·1分", sys.load_ok ? QString::asprintf("%.2f", sys.load1) : "—", "", sys.load_ok ? T::cyan : T::dim);
        stat(p, ix + 2 * (tw + tgap), cy, tw, th, "负载·15分", sys.load_ok ? QString::asprintf("%.2f", sys.load15) : "—", "", T::dim);
        cy += th + fs(20);
        const char *fn[3] = {"A55", "A76-0", "A76-1"};
        for (int i = 0; i < 3; i++)
            stat(p, ix + i * (tw + tgap), cy, tw, th, fn[i], sys.freq_ok[i] ? QString::asprintf("%d", sys.freq[i]) : "—", sys.freq_ok[i] ? "MHz" : "", !sys.freq_ok[i] ? T::dim : i == 0 ? T::green : T::cyan);
    }

    void sparkline(QPainter &p, QRectF r, const std::deque<double> &d, QColor c, double maxv = 100.0) {
        p.setPen(QPen(T::line, 1)); p.setBrush(T::withA(T::bg0, 160)); p.drawRect(r);
        p.setPen(QPen(T::withA(c, 26), 1)); p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        if (d.size() < 2) return;
        QPainterPath path, fill;
        int n = d.size(); double dx = r.width() / (n - 1);
        for (int i = 0; i < n; i++) {
            double v = std::max(0.0, std::min(maxv, d[i]));
            double px = r.left() + i * dx, py = r.bottom() - (v / maxv) * r.height();
            if (i == 0) { path.moveTo(px, py); fill.moveTo(px, r.bottom()); fill.lineTo(px, py); }
            else { path.lineTo(px, py); fill.lineTo(px, py); }
        }
        fill.lineTo(r.right(), r.bottom());
        QLinearGradient g(0, r.top(), 0, r.bottom()); g.setColorAt(0, T::withA(c, 70)); g.setColorAt(1, T::withA(c, 0));
        p.fillPath(fill, g);
        p.setPen(QPen(c, std::max(1, int(fs(1.6))))); p.setBrush(Qt::NoBrush); p.drawPath(path);
    }

    void stat(QPainter &p, double x, double y, double w, double h, const QString &label, const QString &val, const QString &unit, QColor c) {
        QRectF r(x, y, w, h);
        p.setPen(QPen(T::line, 1)); p.setBrush(T::withA(T::bg0, 150)); p.drawRect(r);
        p.fillRect(QRectF(x, y, fs(3), h), c); // accent tab
        p.setFont(cjk(9)); p.setPen(T::dim); p.drawText(QPointF(x + fs(12), y + fs(16)), label);
        p.setFont(mono(24, QFont::Bold)); p.setPen(c);
        int vw = p.boundingRect(0, 0, 999, 99, 0, val).width();
        p.drawText(QPointF(x + fs(12), y + h - fs(12)), val);
        if (!unit.isEmpty()) { p.setFont(mono(11)); p.setPen(T::dim); p.drawText(QPointF(x + fs(12) + vw + fs(6), y + h - fs(12)), unit); }
    }

    void noSignal(QPainter &p, QRect R) {
        reticle(p, R.center().x(), R.top() + R.height() * 0.42, fs(40), false, T::dim, "");
        p.setFont(cjk(20, QFont::Bold, 3)); p.setPen(T::dim);
        p.drawText(R.adjusted(0, int(R.height() * 0.5), 0, 0), Qt::AlignHCenter | Qt::AlignTop, "无 信 号");
        p.setFont(cjk(11)); p.setPen(T::faint.lighter(160));
        p.drawText(R.adjusted(0, int(R.height() * 0.56), 0, 0), Qt::AlignHCenter | Qt::AlignTop, "接入遥测：  tennis-app … | dashboard");
    }

    // Robot's-eye viewport. With a live camera feed it shows the real (low-fps,
    // crossfaded) frame; without one it falls back to a synthetic reconstruction.
    // Either way the detection overlay (aim, ball box at cx/cy, motion-bridge
    // trail, state) runs at telemetry rate, so the fast overlay reads coherently
    // over the slow image — the trail bridges the stale frame to the live target.
    void viewport(QPainter &p, QRectF r) {
        p.save();
        p.setPen(QPen(T::withA(T::line, 200), std::max(1, int(fs(1))))); p.setBrush(QColor(3, 6, 8)); p.drawRect(r);
        p.setClipRect(r);
        bool live = cam.have();
        QRectF ir = r;  // rect the image/scene occupies (camera image is letterboxed)
        if (live) {
            const QImage &img = cam.cur;
            double ar = img.height() ? double(img.width()) / img.height() : 16.0 / 9.0, rar = r.width() / r.height();
            double iw = ar > rar ? r.width() : r.height() * ar, ih = ar > rar ? r.width() / ar : r.height();
            ir = QRectF(r.center().x() - iw / 2, r.center().y() - ih / 2, iw, ih);
            double f = cam.fade();
            if (!cam.prev.isNull() && f < 1.0) p.drawImage(ir, cam.prev);         // crossfade the low-fps feed in
            p.setOpacity(f < 1.0 ? f : 1.0); p.drawImage(ir, img); p.setOpacity(1.0);
            p.fillRect(r, QColor(0, 0, 8, 70));                                   // darken for HUD legibility
        } else {
            double gx = std::max(fs(20), r.width() / 14.0);
            p.setPen(QPen(T::withA(T::cyan, 12), 1));
            for (double x = r.left(); x <= r.right(); x += gx) p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            for (double yy = r.top(); yy <= r.bottom(); yy += gx) p.drawLine(QPointF(r.left(), yy), QPointF(r.right(), yy));
        }
        double cxp = ir.center().x(), midY = ir.center().y();
        p.setPen(QPen(T::withA(T::amber, 90), std::max(1, int(fs(1))), Qt::DashLine)); p.drawLine(QPointF(cxp, ir.top()), QPointF(cxp, ir.bottom()));
        for (int k = 1; k <= 3; k++) { p.setPen(QPen(T::withA(T::green, 48 - k * 8), 1)); p.setBrush(Qt::NoBrush); p.drawEllipse(QPointF(cxp, midY), ir.width() * 0.06 * k, ir.height() * 0.075 * k); }
        double rr = fs(7); p.setPen(QPen(T::withA(T::amber, 160), std::max(1, int(fs(1.2)))));
        p.drawLine(QPointF(cxp - rr, midY), QPointF(cxp + rr, midY)); p.drawLine(QPointF(cxp, midY - rr), QPointF(cxp, midY + rr));
        if (tn.ball && tn.ball_cx >= 0) {
            double nx = tn.ball_cx / tn.frame_w, ny = (tn.ball_cy >= 0 ? tn.ball_cy : tn.frame_h * 0.5) / tn.frame_h;
            double bx = ir.left() + std::max(0.0, std::min(1.0, nx)) * ir.width();
            double by = ir.top() + std::max(0.0, std::min(1.0, ny)) * ir.height();
            double side = std::max(fs(14), std::min(ir.height() * 0.6, std::sqrt(std::max(0.0006, tn.ball_area)) * ir.width() * 0.95));
            for (size_t i = 1; i < tn.ball_hist.size(); i++) {  // motion-bridge trail: stale-frame position (tail) -> now (head)
                double a = double(i) / tn.ball_hist.size();
                QPointF p0(ir.left() + tn.ball_hist[i - 1].x() * ir.width(), ir.top() + tn.ball_hist[i - 1].y() * ir.height());
                QPointF p1(ir.left() + tn.ball_hist[i].x() * ir.width(), ir.top() + tn.ball_hist[i].y() * ir.height());
                p.setPen(QPen(T::withA(T::green, int(150 * a)), std::max(1, int(fs(1.2) * (0.4 + a))))); p.drawLine(p0, p1);
            }
            for (int k = 3; k >= 1; k--) { p.setPen(Qt::NoPen); p.setBrush(T::withA(T::green, 13 * k)); p.drawEllipse(QPointF(bx, by), side * 0.5 + k * fs(2.2), side * 0.5 + k * fs(2.2)); }
            p.setBrush(T::withA(T::green, live ? 24 : 42)); p.setPen(QPen(T::green, std::max(1, int(fs(1.6)))));
            p.drawRect(QRectF(bx - side / 2, by - side / 2, side, side));
            p.setBrush(T::green); p.setPen(Qt::NoPen); p.drawEllipse(QPointF(bx, by), fs(3.2), fs(3.2));
            p.setFont(mono(9, QFont::Bold)); p.setPen(T::green);
            p.drawText(QPointF(bx - side / 2, by - side / 2 - fs(5)), QString::asprintf("球 %.1f%%", tn.ball_area * 100.0));
        }
        p.restore();
        QColor sc = stateColor(tn.state);
        glow(p, cjk(17, QFont::Bold, 1), sc, r.left() + fs(12), r.top() + fs(25), stateCN(tn.state), 70);
        if (tn.bucket) {
            QFont f = cjk(10, QFont::Bold); QString t = "● 球桶可见"; int tw = QFontMetrics(f).horizontalAdvance(t);
            p.setFont(f); p.setPen(T::amber); p.drawText(QPointF(r.right() - tw - fs(12), r.top() + fs(23)), t);
        }
        p.setFont(mono(10)); p.setPen(T::withA(T::ink, 220));
        p.drawText(QPointF(r.left() + fs(12), r.bottom() - fs(12)), tn.ball ? QString::asprintf("偏移 %+.0f px", tn.ball_cx - tn.frame_w * 0.5) : QString("未见目标"));
        QFont wf = cjk(8); QString wm = live ? "摄像头 · 实时叠加" : "重建 · 遥测"; int ww = QFontMetrics(wf).horizontalAdvance(wm);
        p.setFont(wf); p.setPen(T::withA(live ? T::green : T::dim, 175)); p.drawText(QPointF(r.right() - ww - fs(12), r.bottom() - fs(12)), wm);
    }

    // Top-down field map from odometry: robot pose + trail + anchor + ball bearing.
    void radar(QPainter &p, QRectF box) {
        double cx = box.center().x(), cy = box.center().y(), R = std::min(box.width(), box.height()) * 0.47;
        p.setBrush(QColor(3, 6, 8)); p.setPen(QPen(T::withA(T::violet, 120), std::max(1, int(fs(1.2))))); p.drawEllipse(QPointF(cx, cy), R, R);
        p.save(); QPainterPath clip; clip.addEllipse(QPointF(cx, cy), R, R); p.setClipPath(clip);
        for (int k = 1; k <= 3; k++) { p.setPen(QPen(T::withA(T::violet, 45), 1)); p.setBrush(Qt::NoBrush); p.drawEllipse(QPointF(cx, cy), R * k / 3.0, R * k / 3.0); }
        p.setPen(QPen(T::withA(T::violet, 40), 1)); p.drawLine(QPointF(cx - R, cy), QPointF(cx + R, cy)); p.drawLine(QPointF(cx, cy - R), QPointF(cx, cy + R));
        double maxr = 0.6;
        if (tn.odom_valid) {
            for (auto &pt : tn.odom_hist) maxr = std::max(maxr, std::hypot(pt.x(), pt.y()));
            maxr = std::max(maxr, tn.odom_distance) * 1.18;
            auto S = [&](double x, double y) { return QPointF(cx - y / maxr * R, cy - x / maxr * R); };  // x fwd→up, y left→left
            QPointF a = S(0, 0); double ar = fs(6);  // anchor (odometry origin)
            p.setPen(QPen(T::withA(T::amber, 210), std::max(1, int(fs(1.4))))); p.setBrush(Qt::NoBrush);
            p.drawLine(QPointF(a.x() - ar, a.y()), QPointF(a.x() + ar, a.y())); p.drawLine(QPointF(a.x(), a.y() - ar), QPointF(a.x(), a.y() + ar));
            p.drawEllipse(a, ar * 0.66, ar * 0.66);
            if (tn.odom_hist.size() > 1) {  // trail
                QPainterPath tp;
                for (size_t i = 0; i < tn.odom_hist.size(); i++) { QPointF s = S(tn.odom_hist[i].x(), tn.odom_hist[i].y()); if (i == 0) tp.moveTo(s); else tp.lineTo(s); }
                p.setPen(QPen(T::withA(T::cyan, 130), std::max(1, int(fs(1.4))))); p.setBrush(Qt::NoBrush); p.drawPath(tp);
            }
            QPointF rb = S(tn.odom_x, tn.odom_y);
            double sx = -std::sin(tn.odom_heading), sy = -std::cos(tn.odom_heading), al = fs(13);
            if (tn.ball && tn.ball_cx >= 0) {  // ball bearing ray (from cx, ≈±30° HFOV; range unknown → dashed)
                double bth = tn.odom_heading - (tn.ball_cx - 320.0) / 320.0 * (M_PI / 6.0);
                double bx = -std::sin(bth), by = -std::cos(bth);
                p.setPen(QPen(T::withA(T::green, 150), std::max(1, int(fs(1.2))), Qt::DashLine));
                p.drawLine(rb, QPointF(rb.x() + bx * R * 0.85, rb.y() + by * R * 0.85));
                p.setBrush(T::green); p.setPen(Qt::NoPen); p.drawEllipse(QPointF(rb.x() + bx * R * 0.72, rb.y() + by * R * 0.72), fs(3.4), fs(3.4));
            }
            double px = -sy, py = sx;  // robot arrow (points at heading)
            QPointF tip(rb.x() + sx * al, rb.y() + sy * al), base(rb.x() - sx * al * 0.5, rb.y() - sy * al * 0.5);
            for (int k = 3; k >= 1; k--) { p.setPen(Qt::NoPen); p.setBrush(T::withA(T::violet, 20 * k)); p.drawEllipse(rb, al * 0.5 + k * fs(1.5), al * 0.5 + k * fs(1.5)); }
            QPointF tri[3] = {tip, QPointF(base.x() + px * al * 0.5, base.y() + py * al * 0.5), QPointF(base.x() - px * al * 0.5, base.y() - py * al * 0.5)};
            p.setBrush(T::violet); p.setPen(Qt::NoPen); p.drawPolygon(tri, 3);
        }
        p.restore();
        p.setBrush(T::withA(T::violet, 220)); p.setPen(Qt::NoPen);  // forward marker
        QPointF nt[3] = {QPointF(cx, cy - R - fs(5)), QPointF(cx - fs(3.4), cy - R + fs(3)), QPointF(cx + fs(3.4), cy - R + fs(3))}; p.drawPolygon(nt, 3);
        p.setFont(cjk(8, QFont::Bold)); p.setPen(T::withA(T::violet, 210)); p.drawText(QRectF(cx - R, cy - R - fs(18), R * 2, fs(12)), Qt::AlignHCenter, "前");
        if (tn.odom_valid) { p.setFont(mono(8)); p.setPen(T::withA(T::dim, 200)); p.drawText(QRectF(cx - R, cy + R + fs(2), R * 2, fs(12)), Qt::AlignHCenter, QString::asprintf("量程 %.1f m", maxr)); }
    }

    // Column 02 — 机器人视角: reconstructed viewport (hero) + differential drive + control tiles.
    void visionCol(QPainter &p, int x, int y, int w, int h) {
        QRect R(x, y, w, h); frame(p, R, "02", "机器人视角", T::amber);
        int ix = x + int(fs(24)), iw = w - int(fs(48));
        if (!tn.seen) { noSignal(p, R); return; }
        int cy = y + int(fs(68));
        double vpH = h * 0.44;
        viewport(p, QRectF(ix, cy, iw, vpH));
        cy += int(vpH + fs(30));
        p.setFont(cjk(11, QFont::Bold, 2)); p.setPen(T::dim); p.drawText(ix, cy, "差速驱动");
        cy += int(fs(14));
        double mh = h * 0.05;
        p.setFont(cjk(11, QFont::Bold)); p.setPen(T::ink); p.drawText(QPointF(ix, cy + mh * 0.72), "左");
        motor(p, QRectF(ix + fs(24), cy, iw - fs(24) - fs(64), mh), tn.mL);
        p.setFont(mono(12, QFont::Bold)); p.setPen(tn.mL >= 0 ? T::green : T::coral); p.drawText(QRectF(ix + iw - fs(58), cy, fs(58), mh), Qt::AlignRight | Qt::AlignVCenter, QString::asprintf("%+d", tn.mL));
        cy += int(mh + fs(9));
        p.setFont(cjk(11, QFont::Bold)); p.setPen(T::ink); p.drawText(QPointF(ix, cy + mh * 0.72), "右");
        motor(p, QRectF(ix + fs(24), cy, iw - fs(24) - fs(64), mh), tn.mR);
        p.setFont(mono(12, QFont::Bold)); p.setPen(tn.mR >= 0 ? T::green : T::coral); p.drawText(QRectF(ix + iw - fs(58), cy, fs(58), mh), Qt::AlignRight | Qt::AlignVCenter, QString::asprintf("%+d", tn.mR));
        cy += int(mh + fs(28));
        double tgap = fs(12), tw = (iw - tgap * 2) / 3, th = h * 0.115;
        stat(p, ix, cy, tw, th, "机械臂", armCN(tn.arm), "", T::amber);
        stat(p, ix + tw + tgap, cy, tw, th, "帧率", QString::asprintf("%.1f", tn.done ? tn.res_fps : tn.fps), "", T::green);
        stat(p, ix + 2 * (tw + tgap), cy, tw, th, "帧→指令", QString::asprintf("%.1f", tn.f2c), "ms", tn.f2c > 40 ? T::amber : T::cyan);
    }

    // Column 03 — 场地雷达: odometry radar + pose readouts + status tiles + latency trend.
    void navCol(QPainter &p, int x, int y, int w, int h) {
        QRect R(x, y, w, h); frame(p, R, "03", "场地雷达", T::violet);
        int ix = x + int(fs(24)), iw = w - int(fs(48));
        if (!tn.seen) { noSignal(p, R); return; }
        int cy = y + int(fs(68));
        double rdH = std::min(iw * 0.9, h * 0.38);
        radar(p, QRectF(ix, cy, iw, rdH));
        cy += int(rdH + fs(30));
        double cgap = fs(12), cw = (iw - cgap * 2) / 3;
        auto od = [&](double cxp, const QString &label, const QString &val, QColor vc) {
            p.setFont(cjk(9)); p.setPen(T::dim); p.drawText(QPointF(cxp, cy), label);
            p.setFont(mono(16, QFont::Bold)); p.setPen(tn.odom_valid ? vc : T::dim); p.drawText(QPointF(cxp, cy + fs(22)), val);
        };
        od(ix, "距锚点", tn.odom_valid ? QString::asprintf("%.2f", tn.odom_distance) : "—", T::amber);
        od(ix + cw + cgap, "航向", tn.odom_valid ? QString::asprintf("%+.0f°", tn.odom_heading * 180.0 / M_PI) : "—", T::violet);
        od(ix + 2 * (cw + cgap), "坐标", tn.odom_valid ? QString::asprintf("%+.1f,%+.1f", tn.odom_x, tn.odom_y) : "—", T::cyan);
        if (!tn.odom_valid) { p.setFont(cjk(9)); p.setPen(T::withA(T::dim, 170)); p.drawText(QRectF(ix, cy - fs(30), iw, fs(14)), Qt::AlignRight, "锚点未初始化"); }
        cy += int(fs(52));
        double tgap = fs(12), tw = (iw - tgap * 2) / 3, th = h * 0.115;
        stat(p, ix, cy, tw, th, "首帧推理", tn.ttfi < 0 ? "—" : QString::asprintf("%.0f", tn.ttfi), tn.ttfi < 0 ? "" : "ms", T::amber);
        stat(p, ix + tw + tgap, cy, tw, th, "检测数", QString::asprintf("%llu", (unsigned long long)tn.detections), "", T::cyan);
        stat(p, ix + 2 * (tw + tgap), cy, tw, th, "应用内存", tn.rss_kb ? QString::asprintf("%.0f", tn.rss_kb / 1024.0) : "—", tn.rss_kb ? "MB" : "", T::dim);
        cy += int(th + fs(20));
        p.setFont(cjk(11, QFont::Bold, 2)); p.setPen(T::dim); p.drawText(ix, cy, "帧→指令延迟");
        p.setFont(mono(10)); p.setPen(T::withA(T::dim, 200)); p.drawText(QRectF(ix, cy - fs(11), iw, fs(14)), Qt::AlignRight, "ms");
        cy += int(fs(8));
        double rem = (y + h - int(fs(18))) - cy;
        if (rem > fs(30)) sparkline(p, QRectF(ix, cy, iw, std::min(rem, h * 0.14)), tn.f2c_hist, T::amber, 60.0);
    }
};

// Keep the dashboard from ever stealing cycles the tennis app needs. Best-effort;
// each step is optional and silently skipped where unsupported (parts of StarryOS):
//   1. pin to the little (A55) cluster so it never lands on the A76 cores the
//      inference thread uses (--infer-affinity 4-7);
//   2. run SCHED_IDLE so it executes only when a CPU would otherwise be idle — any
//      tennis-app thread preempts it instantly; fall back to nice(19).
static void applyIsolation(const QString &cpulist, bool isolate) {
    if (!isolate) return;
    QString applied;
#ifdef __linux__
    if (!cpulist.isEmpty()) {
        cpu_set_t set; CPU_ZERO(&set); int lo, hi; bool any = false;
        for (const QString &tok : cpulist.split(',', Qt::SkipEmptyParts)) {
            if (std::sscanf(tok.toUtf8().constData(), "%d-%d", &lo, &hi) == 2) { for (int c = lo; c <= hi; c++) { CPU_SET(c, &set); any = true; } }
            else if (std::sscanf(tok.toUtf8().constData(), "%d", &lo) == 1) { CPU_SET(lo, &set); any = true; }
        }
        if (any && sched_setaffinity(0, sizeof set, &set) == 0) applied += " affinity=" + cpulist;
    }
    struct sched_param sp; sp.sched_priority = 0;
    if (sched_setscheduler(0, SCHED_IDLE, &sp) == 0) applied += " sched=IDLE";
    else if (setpriority(PRIO_PROCESS, 0, 19) == 0) applied += " nice=19";
#endif
    std::printf("DASHBOARD_ISOLATION%s\n", applied.isEmpty() ? " (none)" : applied.toUtf8().constData());
    std::fflush(stdout);
}

int main(int argc, char **argv) {
    QString shot, telemetry, camera, cpulist = "0-3"; bool demo = false, isolate = true, directfb = false; int sw = 1920, sh = 1080, fps = 8;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) { shot = argv[++i]; demo = true; }
        else if (!std::strcmp(argv[i], "--size") && i + 1 < argc) { std::sscanf(argv[++i], "%dx%d", &sw, &sh); }  // responsive test: --size 1280x720
        else if (!std::strcmp(argv[i], "--telemetry") && i + 1 < argc) { telemetry = argv[++i]; }  // tail a file instead of stdin
        else if (!std::strcmp(argv[i], "--camera") && i + 1 < argc) { camera = argv[++i]; }         // tmpfs file the app publishes frames to
        else if (!std::strcmp(argv[i], "--cpu") && i + 1 < argc) { cpulist = argv[++i]; }           // affinity list, default A55 0-3
        else if (!std::strcmp(argv[i], "--fps") && i + 1 < argc) { fps = std::atoi(argv[++i]); }    // repaint cap, default 8
        else if (!std::strcmp(argv[i], "--directfb")) { directfb = true; }                          // present via /dev/fb0 (StarryOS)
        else if (!std::strcmp(argv[i], "--no-isolate")) { isolate = false; }
        else if (!std::strcmp(argv[i], "--demo")) demo = true;
    }
    QApplication app(argc, argv);
    // Load bundled display fonts so the HUD looks right regardless of the board's
    // fontconfig: Fira Code (Latin data), Oswald (Latin brand), Noto Sans CJK SC
    // (思源黑体 — Chinese labels). Falls back to system fonts if a file is absent.
    QString ed = QCoreApplication::applicationDirPath();
    for (const QString &fp : {ed + "/fonts/FiraCode-Regular.ttf", ed + "/fonts/FiraCode-Bold.ttf",
                              ed + "/fonts/Oswald.ttf", QStringLiteral("/usr/share/fonts/truetype/hud/Oswald.ttf"),
                              ed + "/fonts/NotoSansCJKsc-Regular.otf", ed + "/fonts/NotoSansCJKsc-Bold.otf",
                              ed + "/fonts/NotoSansCJK-Regular.ttc",
                              QStringLiteral("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
                              QStringLiteral("/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc")})
        QFontDatabase::addApplicationFont(fp);
    if (!shot.isEmpty()) {
        Dashboard d(true); d.resize(sw, sh);
        QPixmap pm = d.grab();
        pm.save(shot);
        std::printf("DASHBOARD_SHOT %s %dx%d\n", shot.toUtf8().constData(), sw, sh);
        return 0;
    }
    if (directfb) {
        // Present to the VOP2 scanout via the UNCACHED /dev/fb0 mmap — NOT write()/read().
        // On StarryOS the read_at/write_at path is a CACHED alias that is incoherent with the
        // DRAM the VOP2 scans (proven in-process: an mmap read shows content while read() shows
        // 0 — the "mismatched-cacheability alias" the VOP2 driver warns about). So we memcpy the
        // offscreen render straight into the uncached mmap, and read the SAME mmap back to verify
        // exactly what the panel receives. Bypasses Qt's linuxfb compositor entirely.
        int fbfd = ::open("/dev/fb0", O_RDWR);
        if (fbfd < 0) { std::fprintf(stderr, "directfb: open /dev/fb0 failed\n"); return 1; }
        struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
        ::ioctl(fbfd, FBIOGET_VSCREENINFO, &vi); ::ioctl(fbfd, FBIOGET_FSCREENINFO, &fi);
        int fbw = vi.xres ? int(vi.xres) : sw, fbh = vi.yres ? int(vi.yres) : sh;
        int bpp = vi.bits_per_pixel ? int(vi.bits_per_pixel) / 8 : 4;
        long stride = fi.line_length ? long(fi.line_length) : long(fbw) * bpp;
        long fbsize = stride * fbh;
        unsigned char *fbmap = (unsigned char *)::mmap(nullptr, size_t(fbsize), PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
        if (fbmap == MAP_FAILED) { std::fprintf(stderr, "directfb: mmap /dev/fb0 failed\n"); return 1; }
        applyIsolation(cpulist, isolate);
        auto *d = new Dashboard(demo, telemetry, fps);
        if (!camera.isEmpty()) d->setCameraPath(camera.toStdString());
        d->resize(fbw, fbh);
        QString dumpPath = ::getenv("STARRY_GRAB_DUMP") ? QString(::getenv("STARRY_GRAB_DUMP")) : QString();
        // STARRY_GRAB_AFTER_FRAMES delays the one-shot DRAM dump by N presents so a
        // *live* feed (camera frames + telemetry that arrive after the first
        // present) is captured, not just frame 0. Frame-counted, not time-based:
        // StarryOS's clock/timers are unreliable under linuxfb. Default 1 = dump on
        // the first present (unchanged one-shot behavior).
        const long grabAfterFrames =
            ::getenv("STARRY_GRAB_AFTER_FRAMES") ? std::atol(::getenv("STARRY_GRAB_AFTER_FRAMES")) : 1;
        auto *dumped = new bool(false);
        auto *framec = new long(0);
        auto *canvas = new QImage(fbw, fbh, QImage::Format_RGB32);  // opaque RGB32; bytesPerLine == stride at 1920 (no padding)
        auto present = [=]() {
            canvas->fill(0xff000000);
            d->render(canvas);  // raster-engine render at the exact fb size, independent of the QScreen
            long total = std::min<long>(fbsize, long(canvas->sizeInBytes()));
            if (long(canvas->bytesPerLine()) == stride) std::memcpy(fbmap, canvas->constBits(), size_t(total));
            else for (int y = 0; y < fbh; y++) std::memcpy(fbmap + size_t(y) * stride, canvas->constScanLine(y), size_t(std::min<long>(stride, long(canvas->bytesPerLine()))));
            ++*framec;
            if (!*dumped && *framec >= grabAfterFrames) {  // verify via the SAME uncached mmap (the truthful DRAM / VOP2 oracle)
                long rbright = 0, mmbright = 0;
                for (int y = 0; y < fbh; y++) { const uchar *s = canvas->constScanLine(y); for (int x = 0; x < fbw * 4; x += 4) if (s[x] >= 0x40 || s[x + 1] >= 0x40 || s[x + 2] >= 0x40) rbright++; }
                for (long i = 0; i + 3 < fbsize; i += 4) if (fbmap[i] >= 0x40 || fbmap[i + 1] >= 0x40 || fbmap[i + 2] >= 0x40) mmbright++;
                std::fprintf(stderr, "DIRECTFB_SELFCHECK render_bright=%ld mmapread_bright=%ld\n", rbright, mmbright); std::fflush(stderr);
                if (!dumpPath.isEmpty()) { FILE *f = ::fopen(dumpPath.toUtf8().constData(), "wb"); if (f) { std::fwrite(fbmap, 1, size_t(fbsize), f); std::fclose(f); } }  // dump the DRAM = exactly what the panel scans
                *dumped = true;
            }
        };
        std::printf("DASHBOARD_DIRECTFB %dx%d bpp=%d stride=%ld (mmap present)\n", fbw, fbh, bpp, stride); std::fflush(stdout);
        // Drive presents from a plain loop, NOT a QTimer: StarryOS's Qt event loop
        // under linuxfb does not reliably fire timers, so the camera poll +
        // telemetry drain (normally timer-driven) are pumped manually here. This is
        // what makes the live feed (and its frame-gated capture) actually update on
        // the board rather than freezing on frame 0.
        const int usPeriod = 1000000 / std::max(1, fps);
        for (;;) {
            QCoreApplication::processEvents();  // service any real Qt events
            d->pump();                          // poll /dev camera file + drain telemetry
            present();                          // render + uncached-mmap present + frame-gated dump
            std::fflush(stdout);
            ::usleep(usPeriod);
        }
    }
    applyIsolation(cpulist, isolate);
    Dashboard d(demo, telemetry, fps);
    if (!camera.isEmpty()) d.setCameraPath(camera.toStdString());
    d.showFullScreen();
    std::printf("DASHBOARD_STARTED\n"); std::fflush(stdout);
    return app.exec();
}
