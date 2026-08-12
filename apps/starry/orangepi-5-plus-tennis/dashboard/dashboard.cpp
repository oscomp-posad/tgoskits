// STARRY//SIGNAL — a phosphor telemetry console for StarryOS / RK3588.
//
// A fullscreen HUD (rendered to the native VOP2 /dev/fb0 via Qt6 linuxfb) that
// fuses live system stats with the tennis-robot app's telemetry. Aesthetic: an
// avionics / oscilloscope readout — scope graticule, corner-bracket framing,
// monospace telemetry, segmented bargraphs, center-zero motor meters, crosshair
// detection reticles, CRT bloom.
//
// Data:
//   system  — procfs/sysfs, 1 Hz (/proc/stat per-core, meminfo, loadavg, uptime,
//             thermal_zone0, cpufreq policy0/4/6 for A55 / A76x2)
//   robot   — the app's TENNIS_* stdout telemetry, read from STDIN (non-blocking),
//             so `tennis-app … | dashboard` shows both; `dashboard` alone = system.
//
// Run fullscreen:  QT_QPA_PLATFORM=linuxfb ./dashboard
// Screenshot (host, offscreen):  QT_QPA_PLATFORM=offscreen ./dashboard --shot out.png
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QTimer>
#include <QSocketNotifier>
#include <QFontDatabase>
#include <QDateTime>
#include <QElapsedTimer>
#include <array>
#include <deque>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

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
QString MONO = "Fira Code";
QString DISP = "Oswald";
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
    double temp = 0;
    std::array<int, 3> freq{};
    double load1 = 0, load5 = 0, load15 = 0;
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
        temp = t.empty() ? 0 : std::strtod(t.c_str(), nullptr) / 1000.0;
        const char *pol[3] = {"/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq",
                              "/sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq",
                              "/sys/devices/system/cpu/cpufreq/policy6/scaling_cur_freq"};
        for (int i = 0; i < 3; i++) { std::string f = slurp(pol[i]); freq[i] = f.empty() ? 0 : int(std::strtol(f.c_str(), nullptr, 10) / 1000); }
        std::sscanf(slurp("/proc/loadavg").c_str(), "%lf %lf %lf", &load1, &load5, &load15);
        uptime = uint64_t(std::strtod(slurp("/proc/uptime").c_str(), nullptr));
    }
};

// ------------------------------ tennis stats --------------------------------
struct TennisStats {
    bool seen = false;
    std::string state = "STANDBY";
    bool ball = false, bucket = false;
    double frame_age = 0;
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
            frame_age = kvd(l, "frame_age_ms");
            uint64_t f = kvl(l, "frame"); double w = nowSec();
            if (frames && f > last_f && w > last_w) { double i = (f - last_f) / (w - last_w); fps = fps == 0 ? i : fps * 0.7 + i * 0.3; }
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

// ------------------------------ dashboard -----------------------------------
class Dashboard : public QWidget {
public:
    explicit Dashboard(bool demo = false) {
        sys.sampleCpu(); sys.sampleRest();
        if (demo) injectDemo();
        else {
            auto *t = new QTimer(this);
            connect(t, &QTimer::timeout, this, [this] { sys.sampleCpu(); sys.sampleRest(); update(); });
            t->start(1000);
            ::fcntl(0, F_SETFL, ::fcntl(0, F_GETFL) | O_NONBLOCK);
            auto *sn = new QSocketNotifier(0, QSocketNotifier::Read, this);
            connect(sn, &QSocketNotifier::activated, this, [this] { drain(); });
            phase.start();
            auto *blink = new QTimer(this);
            connect(blink, &QTimer::timeout, this, [this] { update(); });
            blink->start(120);
        }
    }
    void injectDemo() {
        double v[8] = {22, 14, 9, 31, 88, 74, 61, 45};
        for (int i = 0; i < 8; i++) sys.cpu[i] = v[i];
        sys.aggregate = 43; sys.mem_pct = 38; sys.mem_used_gb = 3.0; sys.mem_total_gb = 7.7;
        sys.temp = 58.4; sys.freq = {1800, 2256, 2256}; sys.load1 = 2.31; sys.load5 = 1.9; sys.load15 = 1.4;
        sys.uptime = 4293; sys.have = true;
        for (int i = 0; i < 120; i++) sys.hist.push_back(38 + 30 * std::sin(i * 0.21) + 12 * std::sin(i * 0.63) + (i % 7) * 1.5);
        tn.seen = true; tn.state = "CHASE_BALL"; tn.ball = true; tn.bucket = false; tn.frame_age = 11.4;
        tn.mL = 42; tn.mR = -42; tn.arm = "None"; tn.f2c = 23.7; tn.ttfi = 8420; tn.rss_kb = 41216;
        tn.frames = 1873; tn.detections = 512; tn.fps = 29.6;
        for (int i = 0; i < 120; i++) tn.f2c_hist.push_back(22 + 9 * std::sin(i * 0.28) + 5 * std::sin(i * 0.91) + (i % 5) * 0.8);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const int W = width(), H = height();
        background(p, W, H);
        int m = std::max(14, W / 80);
        int hh = std::max(56, H / 15);
        topbar(p, m, m, W - 2 * m, hh);
        int top = m + hh + m;
        int gap = m;
        int lw = int((W - 3 * gap) * 0.505);
        int rw = W - 3 * gap - lw;
        int bh = H - top - m;
        systemPanel(p, gap, top, lw, bh);
        robotPanel(p, gap * 2 + lw, top, rw, bh);
    }

private:
    SystemStats sys; TennisStats tn; std::string buf;
    QElapsedTimer phase;
    double fs(double b) const { return b * height() / 1080.0; }
    QFont mono(double pt, int weight = QFont::Medium) const { QFont f(T::MONO); f.setPointSizeF(fs(pt)); f.setWeight(QFont::Weight(weight)); return f; }
    QFont disp(double pt, double spacing = 3.0) const { QFont f(T::DISP); f.setPointSizeF(fs(pt)); f.setBold(true); f.setLetterSpacing(QFont::AbsoluteSpacing, fs(spacing)); f.setCapitalization(QFont::AllUppercase); return f; }

    void drain() {
        char b[8192]; ssize_t n;
        while ((n = ::read(0, b, sizeof b)) > 0) buf.append(b, size_t(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) { tn.feed(buf.substr(0, nl)); buf.erase(0, nl + 1); }
        update();
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
        // header label
        int y = r.top() + int(fs(30));
        p.setFont(mono(12, QFont::Bold)); p.setPen(accent);
        p.drawText(r.left() + int(fs(20)), y, idx);
        QRect ib = p.boundingRect(r.left() + int(fs(20)), y, r.width(), int(fs(20)), Qt::AlignLeft, idx);
        p.setFont(disp(15, 4)); p.setPen(T::ink);
        p.drawText(ib.right() + int(fs(14)), y, title);
        // right-side tick ruler
        p.setPen(QPen(T::line, 1));
        for (int i = 0; i < 6; i++) { int tx = r.right() - int(fs(20)) - i * int(fs(12)); p.drawLine(tx, r.top() + int(fs(14)), tx, r.top() + int(fs(14)) + (i % 2 ? int(fs(6)) : int(fs(10)))); }
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
        p.setFont(mono(12, on ? QFont::Bold : QFont::Normal)); p.setPen(on ? T::ink : T::dim);
        p.drawText(QPointF(cx + rad + fs(16), cy + fs(5)), label);
    }

    void glow(QPainter &p, const QFont &f, QColor c, double x, double y, const QString &s, int a = 60) {
        p.setFont(f);
        p.setPen(T::withA(c, a)); p.drawText(QPointF(x, y), s);
        p.setPen(c); p.drawText(QPointF(x, y), s);
    }

    void topbar(QPainter &p, int x, int y, int w, int h) {
        QRect r(x, y, w, h);
        p.setPen(QPen(T::line, 1)); p.setBrush(T::withA(T::bg1, 200)); p.drawRect(r);
        p.setFont(disp(24, 5)); glow(p, disp(24, 5), T::amber, x + fs(20), y + h * 0.66, "STARRY//SIGNAL", 70);
        int wm = p.boundingRect(0, 0, w, h, 0, "STARRY//SIGNAL").width();
        double sx = x + fs(20) + wm + fs(34);
        p.setPen(QPen(T::withA(T::amber, 90), 1)); p.drawLine(QPointF(sx - fs(18), y + h * 0.30), QPointF(sx - fs(18), y + h * 0.72));
        p.setFont(mono(11)); p.setPen(T::dim);
        p.drawText(QPointF(sx, y + h * 0.64), "RK3588 · ORANGEPI-5-PLUS · AARCH64");
        // right cluster, laid out right-to-left: [clock] [uptime] [LIVE] [dot]
        double yb = y + h * 0.62;
        double cur = x + w - fs(24);
        QString clk = QDateTime::currentDateTime().toString("HH:mm:ss");
        p.setFont(mono(17, QFont::Bold)); int cw = p.boundingRect(0, 0, w, h, 0, clk).width();
        cur -= cw; p.setPen(T::ink); p.drawText(QPointF(cur, yb), clk); cur -= fs(30);
        char up[48]; std::snprintf(up, sizeof up, "UP %llu:%02llu:%02llu", (unsigned long long)(sys.uptime / 3600), (unsigned long long)((sys.uptime / 60) % 60), (unsigned long long)(sys.uptime % 60));
        p.setFont(mono(11)); int uw = p.boundingRect(0, 0, w, h, 0, up).width();
        cur -= uw; p.setPen(T::dim); p.drawText(QPointF(cur, yb), up); cur -= fs(26);
        bool blink = phase.isValid() ? ((phase.elapsed() / 600) % 2 == 0) : true;
        p.setFont(mono(11, QFont::Bold)); int lw = p.boundingRect(0, 0, w, h, 0, "LIVE").width();
        cur -= lw; p.setPen(blink ? T::coral : T::dim); p.drawText(QPointF(cur, yb), "LIVE"); cur -= fs(16);
        p.setBrush(blink ? T::coral : T::withA(T::coral, 70)); p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(cur - fs(4), y + h * 0.54), fs(6), fs(6));
        p.setBrush(Qt::NoBrush);
    }

    void systemPanel(QPainter &p, int x, int y, int w, int h) {
        QRect R(x, y, w, h);
        frame(p, R, "01", "SYSTEM", T::cyan);
        int ix = x + int(fs(26)), iw = w - int(fs(52));
        int cy = y + int(fs(64));
        p.setFont(disp(12, 3)); p.setPen(T::dim); p.drawText(ix, cy, "CPU  ·  8 CORES  ·  BIG.LITTLE");
        cy += int(fs(30));
        double rh = h * 0.042, rgap = fs(6), csep = fs(15);
        for (int c = 0; c < 8; c++) {
            double ry = cy + c * (rh + rgap) + (c >= 4 ? csep : 0);
            bool little = c < 4; QColor acc = little ? T::green : T::cyan;
            if (c == 0 || c == 4) {  // cluster tag on the left
                p.setFont(mono(9, QFont::Bold)); p.setPen(T::withA(acc, 200));
                p.drawText(QPointF(ix, ry - fs(6)), little ? "4× A55 · EFFICIENCY" : "4× A76 · PERFORMANCE");
            }
            p.setFont(mono(11, QFont::Bold)); p.setPen(acc);
            p.drawText(QPointF(ix, ry + rh * 0.72), QString::asprintf("C%d", c));
            double bx = ix + fs(40), bw = iw - fs(40) - fs(64);
            seg(p, QRectF(bx, ry, bw, rh), sys.cpu[c] / 100.0, acc, 26);
            p.setFont(mono(13, QFont::Bold)); p.setPen(T::load(sys.cpu[c], T::ink, T::amber, T::coral));
            p.drawText(QRectF(ix + iw - fs(60), ry, fs(60), rh), Qt::AlignRight | Qt::AlignVCenter, QString::asprintf("%3.0f%%", sys.cpu[c]));
        }
        cy += 8 * (rh + rgap) + csep + fs(16);
        // aggregate sparkline
        p.setFont(disp(12, 3)); p.setPen(T::dim); p.drawText(ix, cy, "AGGREGATE LOAD");
        p.setFont(mono(13, QFont::Bold)); p.setPen(T::cyan);
        p.drawText(QRectF(ix, cy - fs(12), iw, fs(16)), Qt::AlignRight, QString::asprintf("%.0f%%", sys.aggregate));
        cy += fs(8);
        QRectF spark(ix, cy, iw, h * 0.09);
        sparkline(p, spark, sys.hist, T::cyan);
        cy += spark.height() + fs(22);
        // memory
        p.setFont(disp(12, 3)); p.setPen(T::dim); p.drawText(ix, cy, "MEMORY");
        cy += fs(10);
        seg(p, QRectF(ix, cy, iw, h * 0.036), sys.mem_pct / 100.0, T::green, 40);
        cy += h * 0.036 + fs(20);
        p.setFont(mono(12)); p.setPen(T::ink);
        p.drawText(QPointF(ix, cy), QString::asprintf("%.1f / %.1f GiB", sys.mem_used_gb, sys.mem_total_gb));
        p.setPen(T::dim); p.drawText(QRectF(ix, cy - fs(12), iw, fs(16)), Qt::AlignRight, QString::asprintf("%.0f%% used", sys.mem_pct));
        cy += fs(22);
        // stat tiles: temp / load / freqs
        double tgap = fs(14); double tw = (iw - tgap * 2) / 3, th = h * 0.115;
        stat(p, ix, cy, tw, th, "SOC TEMP", QString::asprintf("%.1f", sys.temp), "°C", sys.temp > 80 ? T::coral : sys.temp > 65 ? T::amber : T::green);
        stat(p, ix + tw + tgap, cy, tw, th, "LOAD 1m", QString::asprintf("%.2f", sys.load1), "", T::cyan);
        stat(p, ix + 2 * (tw + tgap), cy, tw, th, "LOAD 15m", QString::asprintf("%.2f", sys.load15), "", T::dim);
        cy += th + fs(14);
        const char *fn[3] = {"A55", "A76-0", "A76-1"};
        for (int i = 0; i < 3; i++)
            stat(p, ix + i * (tw + tgap), cy, tw, th, fn[i], QString::asprintf("%d", sys.freq[i]), "MHz", i == 0 ? T::green : T::cyan);
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
        p.setFont(mono(9)); p.setPen(T::dim); p.drawText(QPointF(x + fs(12), y + fs(16)), label);
        p.setFont(mono(24, QFont::Bold)); p.setPen(c);
        int vw = p.boundingRect(0, 0, 999, 99, 0, val).width();
        p.drawText(QPointF(x + fs(12), y + h - fs(12)), val);
        if (!unit.isEmpty()) { p.setFont(mono(11)); p.setPen(T::dim); p.drawText(QPointF(x + fs(12) + vw + fs(6), y + h - fs(12)), unit); }
    }

    void robotPanel(QPainter &p, int x, int y, int w, int h) {
        QRect R(x, y, w, h);
        frame(p, R, "02", "TENNIS ROBOT", T::amber);
        int ix = x + int(fs(26)), iw = w - int(fs(52));
        if (!tn.seen) {
            p.setFont(disp(20, 4)); p.setPen(T::dim);
            reticle(p, x + w / 2.0, y + h * 0.42, fs(40), false, T::dim, "");
            p.drawText(R.adjusted(0, int(h * 0.5), 0, 0), Qt::AlignHCenter | Qt::AlignTop, "NO SIGNAL");
            p.setFont(mono(11)); p.setPen(T::faint.lighter(160));
            p.drawText(R.adjusted(0, int(h * 0.56), 0, 0), Qt::AlignHCenter | Qt::AlignTop, "pipe telemetry:  tennis-app … | dashboard");
            return;
        }
        int cy = y + int(fs(66));
        // STATE big readout
        QColor sc = stateColor(tn.state);
        p.setFont(mono(10)); p.setPen(T::dim); p.drawText(QPointF(ix, cy), "STATE MACHINE");
        cy += int(fs(68));
        glow(p, disp(46, 4), sc, ix, cy, QString::fromStdString(tn.state), 80);
        cy += int(fs(20));
        p.setPen(QPen(T::withA(sc, 120), 1)); p.drawLine(QPointF(ix, cy), QPointF(ix + iw, cy));
        cy += int(fs(30));
        // detection reticles
        reticle(p, ix + fs(22), cy, fs(20), tn.ball, T::green, "BALL DETECTED");
        reticle(p, ix + iw * 0.52 + fs(22), cy, fs(20), tn.bucket, T::amber, "BUCKET VISIBLE");
        cy += int(fs(56));
        // motors
        p.setFont(disp(12, 3)); p.setPen(T::dim); p.drawText(ix, cy, "DIFFERENTIAL DRIVE  ·  −100 … +100");
        cy += int(fs(14));
        double mh = h * 0.05;
        p.setFont(mono(11, QFont::Bold)); p.setPen(T::ink); p.drawText(QPointF(ix, cy + mh * 0.72), "L");
        motor(p, QRectF(ix + fs(26), cy, iw - fs(26) - fs(70), mh), tn.mL);
        p.setPen(tn.mL >= 0 ? T::green : T::coral); p.drawText(QRectF(ix + iw - fs(64), cy, fs(64), mh), Qt::AlignRight | Qt::AlignVCenter, QString::asprintf("%+d", tn.mL));
        cy += mh + fs(10);
        p.setPen(T::ink); p.drawText(QPointF(ix, cy + mh * 0.72), "R");
        motor(p, QRectF(ix + fs(26), cy, iw - fs(26) - fs(70), mh), tn.mR);
        p.setPen(tn.mR >= 0 ? T::green : T::coral); p.drawText(QRectF(ix + iw - fs(64), cy, fs(64), mh), Qt::AlignRight | Qt::AlignVCenter, QString::asprintf("%+d", tn.mR));
        cy += mh + fs(24);
        // metric tiles
        double tgap = fs(14); double tw = (iw - tgap * 2) / 3, th = h * 0.115;
        stat(p, ix, cy, tw, th, "ARM", QString::fromStdString(tn.arm), "", T::amber);
        stat(p, ix + tw + tgap, cy, tw, th, "FPS", QString::asprintf("%.1f", tn.done ? tn.res_fps : tn.fps), "", T::green);
        stat(p, ix + 2 * (tw + tgap), cy, tw, th, "FRAME→CMD", QString::asprintf("%.1f", tn.f2c), "ms", tn.f2c > 40 ? T::amber : T::cyan);
        cy += th + fs(14);
        stat(p, ix, cy, tw, th, "TTFI", tn.ttfi < 0 ? "—" : QString::asprintf("%.0f", tn.ttfi), tn.ttfi < 0 ? "" : "ms", T::amber);
        stat(p, ix + tw + tgap, cy, tw, th, "DETECTIONS", QString::asprintf("%llu", (unsigned long long)tn.detections), "", T::cyan);
        stat(p, ix + 2 * (tw + tgap), cy, tw, th, "APP RSS", tn.rss_kb ? QString::asprintf("%.0f", tn.rss_kb / 1024.0) : "—", tn.rss_kb ? "MB" : "", T::dim);
        cy += th + fs(20);
        p.setFont(mono(10)); p.setPen(T::dim);
        p.drawText(QPointF(ix, cy), QString::asprintf("frame age %.1f ms   ·   frames %llu%s", tn.frame_age, (unsigned long long)tn.frames, tn.done ? "   ·   RUN COMPLETE" : ""));
        cy += int(fs(24));
        // frame->command latency history fills the remaining space
        p.setFont(disp(12, 3)); p.setPen(T::dim); p.drawText(ix, cy, "FRAME → COMMAND LATENCY  ·  ms");
        p.setFont(mono(11, QFont::Bold)); p.setPen(T::amber);
        p.drawText(QRectF(ix, cy - fs(12), iw, fs(16)), Qt::AlignRight, QString::asprintf("%.1f", tn.f2c));
        cy += int(fs(10));
        double rem = (y + h - int(fs(24))) - cy;
        if (rem > fs(40)) sparkline(p, QRectF(ix, cy, iw, std::min(rem, h * 0.13)), tn.f2c_hist, T::amber, 60.0);
    }
};

int main(int argc, char **argv) {
    QString shot; bool demo = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) { shot = argv[++i]; demo = true; }
        else if (!std::strcmp(argv[i], "--demo")) demo = true;
    }
    QApplication app(argc, argv);
    // Load bundled display fonts so the HUD looks right regardless of the
    // board's fontconfig. Falls back to defaults if absent.
    QString ed = QCoreApplication::applicationDirPath();
    for (const QString &fp : {ed + "/fonts/FiraCode-Regular.ttf", ed + "/fonts/FiraCode-Bold.ttf",
                              ed + "/fonts/Oswald.ttf", QStringLiteral("/usr/share/fonts/truetype/hud/Oswald.ttf")})
        QFontDatabase::addApplicationFont(fp);
    if (!shot.isEmpty()) {
        Dashboard d(true); d.resize(1920, 1080);
        QPixmap pm = d.grab();
        pm.save(shot);
        std::printf("DASHBOARD_SHOT %s\n", shot.toUtf8().constData());
        return 0;
    }
    Dashboard d(demo);
    d.showFullScreen();
    std::printf("DASHBOARD_STARTED\n"); std::fflush(stdout);
    return app.exec();
}
