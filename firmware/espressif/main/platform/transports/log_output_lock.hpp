// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef MICROPIXEL_PLATFORM_TRANSPORTS_LOG_OUTPUT_LOCK_HPP
#define MICROPIXEL_PLATFORM_TRANSPORTS_LOG_OUTPUT_LOCK_HPP

#include <stdio.h>

#include "esp_private/log_lock.h"

namespace micropixel::platform::transports {

// A transport that shares its byte stream with ESP_LOG has to keep log output out
// while it writes one framed response. Two locks guard that output and both are
// needed, because they are taken by different loggers:
//
//   * `esp_log_impl_lock()` is the log mutex the level machinery and the binary
//     logger take (`log_format_binary.c` holds it across the write), and
//   * stdout's FILE lock is what the text logger holds for a whole formatted line
//     (`log_format_text.c` calls flockfile(stdout) around it), which is the mode a
//     default build uses.
//
// Taking only the log mutex therefore leaves the default logger free to interleave
// a line with the frame. The order here matches the binary logger, which takes the
// log mutex first, and the text logger never holds the FILE lock while taking the
// log mutex, so the two locks cannot invert.
inline void LockLogOutput() {
    esp_log_impl_lock();
    flockfile(stdout);
}

inline void UnlockLogOutput() {
    funlockfile(stdout);
    esp_log_impl_unlock();
}

}  // namespace micropixel::platform::transports

#endif  // MICROPIXEL_PLATFORM_TRANSPORTS_LOG_OUTPUT_LOCK_HPP
