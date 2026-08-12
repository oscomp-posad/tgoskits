// SPDX-License-Identifier: Apache-2.0
#include "pwm_motor_backend.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace tennis {
namespace {

constexpr int kPeriodNs = 1000000;

bool exists(const std::string &path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

bool write_value(const std::string &path, int value) {
    std::ofstream file(path);
    if (!file.is_open()) {
        std::fprintf(stderr, "TENNIS_ERROR open %s failed: %s\n", path.c_str(),
                     std::strerror(errno));
        return false;
    }
    file << value;
    if (!file.good()) {
        std::fprintf(stderr, "TENNIS_ERROR write %s failed\n", path.c_str());
        return false;
    }
    return true;
}

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

} // namespace

PwmMotorBackend::PwmMotorBackend(const std::string &spec) {
    if (!parse(spec)) return;
    for (auto &output : outputs_) {
        if (!initialize(output)) return;
    }
    ready_ = true;
    if (!standby()) ready_ = false;
}

PwmMotorBackend::~PwmMotorBackend() {
    if (ready_) (void)standby();
}

bool PwmMotorBackend::parse(const std::string &spec) {
    std::string value = spec;
    if (value.compare(0, 4, "pwm:") == 0) value.erase(0, 4);
    std::stringstream stream(value);
    std::vector<std::string> paths;
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = trim(item);
        const auto equals = item.find('=');
        if (equals != std::string::npos) item.erase(0, equals + 1);
        if (!item.empty() && item.back() != '/') item.push_back('/');
        if (!item.empty()) paths.push_back(item);
    }
    if (paths.size() != outputs_.size()) {
        std::fprintf(stderr,
                     "TENNIS_ERROR PWM spec requires four pwmchip paths: %s\n",
                     spec.c_str());
        return false;
    }
    for (size_t i = 0; i < outputs_.size(); ++i) {
        outputs_[i].chip = paths[i];
        outputs_[i].channel = paths[i] + "pwm0/";
    }
    return true;
}

bool PwmMotorBackend::initialize(Output &output) {
    if (!exists(output.channel)) {
        if (!write_value(output.chip + "export", 0)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!exists(output.channel)) {
        std::fprintf(stderr, "TENNIS_ERROR PWM channel unavailable: %s\n",
                     output.channel.c_str());
        return false;
    }
    return write_value(output.channel + "enable", 0) &&
           write_value(output.channel + "period", kPeriodNs) &&
           write_value(output.channel + "duty_cycle", 0);
}

bool PwmMotorBackend::set_output(Index index, bool enable, int speed) {
    const auto &output = outputs_[static_cast<size_t>(index)];
    const int duty = enable ? std::clamp(speed, 0, 100) * kPeriodNs / 100 : 0;
    return write_value(output.channel + "duty_cycle", duty) &&
           write_value(output.channel + "enable", enable ? 1 : 0);
}

bool PwmMotorBackend::set_pair(Index forward, Index backward, int speed) {
    if (speed > 0) {
        const bool stopped = set_output(backward, false, 0);
        return set_output(forward, true, speed) && stopped;
    } else if (speed < 0) {
        const bool stopped = set_output(forward, false, 0);
        return set_output(backward, true, -speed) && stopped;
    } else {
        const bool forward_stopped = set_output(forward, false, 0);
        return set_output(backward, false, 0) && forward_stopped;
    }
}

bool PwmMotorBackend::drive(int left, int right) {
    const bool left_ok = set_pair(LeftForward, LeftBackward, left);
    const int compensated = right + (right > 0 ? 2 : right < 0 ? -2 : 0);
    return set_pair(RightForward, RightBackward, compensated) && left_ok;
}

bool PwmMotorBackend::brake() {
    bool ok = set_output(LeftForward, true, 100);
    ok = set_output(LeftBackward, true, 100) && ok;
    ok = set_output(RightForward, true, 100) && ok;
    return set_output(RightBackward, true, 100) && ok;
}

bool PwmMotorBackend::standby() {
    bool ok = set_output(LeftForward, false, 0);
    ok = set_output(LeftBackward, false, 0) && ok;
    ok = set_output(RightForward, false, 0) && ok;
    return set_output(RightBackward, false, 0) && ok;
}

} // namespace tennis
