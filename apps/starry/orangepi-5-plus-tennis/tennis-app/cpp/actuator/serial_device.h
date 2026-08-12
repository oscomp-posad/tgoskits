// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tennis {

class SerialDevice {
public:
    SerialDevice() = default;
    ~SerialDevice();

    SerialDevice(const SerialDevice &) = delete;
    SerialDevice &operator=(const SerialDevice &) = delete;

    bool open(const std::string &path, int baudrate = 115200,
              bool nonblocking = true);
    bool write_all(const void *data, size_t size);
    bool read_byte(uint8_t &byte, int timeout_ms);
    bool drain();
    void flush();
    void flush_input();
    bool is_open() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

} // namespace tennis
