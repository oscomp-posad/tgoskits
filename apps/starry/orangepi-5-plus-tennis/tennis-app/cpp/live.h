// SPDX-License-Identifier: Apache-2.0
//
// Board-only entry points (live pipeline + test modes). Defined in live.cc,
// which is excluded from the host dry-run build (it pulls in libuvc/librknnrt).
#pragma once

#include "app_options.h"

namespace tennis {

int run_live(const Options &opts);
int run_test_uvc(const Options &opts);
int run_test_yolo(const Options &opts);
int run_test_bucket(const Options &opts);

} // namespace tennis
