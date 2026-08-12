// Stock-style Qt6 analog clock for StarryOS linuxfb bring-up.
// Prints a heartbeat each second and exits 0 after MAX_TICKS so the
// harness can assert a deterministic pass.
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QTime>
#include <QTimer>
#include <cstdio>

static const int MAX_TICKS = 5;

class Clock : public QWidget {
public:
    Clock() {
        auto *timer = new QTimer(this);
        // repaint + heartbeat every second
        connect(timer, &QTimer::timeout, this, [this]() {
            update();
            std::printf("QT_DEMO_TICK %d\n", ++ticks_);
            std::fflush(stdout);
            if (ticks_ >= MAX_TICKS) {
                std::printf("QT_DEMO_PASSED\n");
                std::fflush(stdout);
                QApplication::quit();
            }
        });
        timer->start(1000);
        setWindowTitle("StarryOS Qt Clock");
    }

protected:
    void paintEvent(QPaintEvent *) override {
        static const QPoint hourHand[3]   = {QPoint(7, 8), QPoint(-7, 8), QPoint(0, -40)};
        static const QPoint minuteHand[3] = {QPoint(7, 8), QPoint(-7, 8), QPoint(0, -70)};
        const QColor hourColor(127, 0, 127);
        const QColor minuteColor(0, 127, 127, 191);

        int side = qMin(width(), height());
        QTime time = QTime::currentTime();

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), Qt::white);
        painter.translate(width() / 2, height() / 2);
        painter.scale(side / 200.0, side / 200.0);

        painter.setPen(Qt::NoPen);
        painter.setBrush(hourColor);
        painter.save();
        painter.rotate(30.0 * ((time.hour() + time.minute() / 60.0)));
        painter.drawConvexPolygon(hourHand, 3);
        painter.restore();

        painter.setBrush(minuteColor);
        painter.save();
        painter.rotate(6.0 * (time.minute() + time.second() / 60.0));
        painter.drawConvexPolygon(minuteHand, 3);
        painter.restore();

        painter.setPen(hourColor);
        for (int i = 0; i < 12; ++i) {
            painter.drawLine(88, 0, 96, 0);
            painter.rotate(30.0);
        }
    }

private:
    int ticks_ = 0;
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    Clock clock;
    clock.resize(400, 400);
    clock.showFullScreen();
    std::printf("QT_DEMO_STARTED\n");
    std::fflush(stdout);
    return app.exec();
}
