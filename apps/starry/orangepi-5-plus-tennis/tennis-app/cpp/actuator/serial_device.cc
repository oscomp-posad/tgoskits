// SPDX-License-Identifier: Apache-2.0
#include "serial_device.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace tennis {

SerialDevice::~SerialDevice() {
    if (fd_ >= 0) ::close(fd_);
}

bool SerialDevice::open(const std::string &path, int baudrate,
                        bool nonblocking) {
    if (baudrate != 115200) {
        std::fprintf(stderr, "TENNIS_ERROR unsupported serial baudrate: %d\n",
                     baudrate);
        return false;
    }

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        std::fprintf(stderr, "TENNIS_ERROR open %s failed: %s\n", path.c_str(),
                     std::strerror(errno));
        return false;
    }

    termios tty{};
    if (tcgetattr(fd_, &tty) != 0) {
        std::fprintf(stderr, "TENNIS_ERROR tcgetattr %s failed: %s\n",
                     path.c_str(), std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK |
                     ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST;
    tty.c_lflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    tcflush(fd_, TCIOFLUSH);
    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        std::fprintf(stderr, "TENNIS_ERROR tcsetattr %s failed: %s\n",
                     path.c_str(), std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (!nonblocking && fcntl(fd_, F_SETFL, 0) != 0) {
        std::fprintf(stderr, "TENNIS_ERROR configure %s blocking mode failed: %s\n",
                     path.c_str(), std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

bool SerialDevice::write_all(const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(fd_, bytes + offset, size - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set writefds;
            FD_ZERO(&writefds);
            FD_SET(fd_, &writefds);
            timeval timeout{0, 500000};
            if (select(fd_ + 1, nullptr, &writefds, nullptr, &timeout) > 0)
                continue;
        }
        std::fprintf(stderr, "TENNIS_ERROR serial write failed: %s\n",
                     std::strerror(errno));
        return false;
    }
    return true;
}

bool SerialDevice::read_byte(uint8_t &byte, int timeout_ms) {
    for (;;) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd_, &readfds);
        timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        const int ready = select(fd_ + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready < 0 && errno == EINTR) continue;
        return ready > 0 && ::read(fd_, &byte, 1) == 1;
    }
}

bool SerialDevice::drain() {
    return tcdrain(fd_) == 0;
}

void SerialDevice::flush() {
    tcflush(fd_, TCIOFLUSH);
}

void SerialDevice::flush_input() {
    tcflush(fd_, TCIFLUSH);
}

} // namespace tennis
