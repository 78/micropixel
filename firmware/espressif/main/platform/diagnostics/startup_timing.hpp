// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "sdkconfig.h"

#if CONFIG_MICROPIXEL_BOARD_ESP_MOSAICO
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>

#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_rtc_time.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "platform/memory/ext_ram_bss.hpp"
#include "soc/rtc.h"
#endif

namespace micropixel::platform::diagnostics {

#if CONFIG_MICROPIXEL_BOARD_ESP_MOSAICO
namespace detail {

struct StartupMark final {
    const char* label{};
    int64_t timestamp_us{};
};

struct StartupTimingState final {
    std::array<StartupMark, 24> marks{};
    size_t count{};
    uint64_t app_main_rtc_api_us{};
    uint64_t app_main_raw_rtc_ticks{};
    uint64_t app_main_raw_rtc_estimate_us{};
    uint32_t slowclk_cal{};
    uint32_t app_main_log_ms{};
    esp_reset_reason_t reset_reason{};
    bool reported{};
    bool overflow{};
};

inline StartupTimingState& StartupTiming() {
    // Only app_main and its synchronous startup callees access this state.
    static MICROPIXEL_EXT_RAM_BSS StartupTimingState state;
    return state;
}

}  // namespace detail

inline void BeginStartupTiming() {
    auto& state = detail::StartupTiming();
    if (state.count != 0U) return;
    state.marks[0].timestamp_us = esp_timer_get_time();
    state.marks[0].label = "app_main";
    state.count = 1U;
    state.app_main_raw_rtc_ticks = rtc_time_get();
    state.slowclk_cal = esp_clk_slowclk_cal_get();
    // Apply the current RC calibration to raw power-domain ticks. This is a
    // coarse cold-start estimate only with POWERON and nonzero calibration;
    // the counter persists across warm resets and earlier RC frequency varies.
    const uint64_t ticks_low = state.app_main_raw_rtc_ticks & UINT32_MAX;
    const uint64_t ticks_high = state.app_main_raw_rtc_ticks >> 32U;
    state.app_main_raw_rtc_estimate_us = ((ticks_low * state.slowclk_cal) >> RTC_CLK_CAL_FRACT) +
                                         ((ticks_high * state.slowclk_cal) << (32U - RTC_CLK_CAL_FRACT));
    state.app_main_rtc_api_us = esp_rtc_get_time_us();
    state.app_main_log_ms = esp_log_timestamp();
    state.reset_reason = esp_reset_reason();
}

// Labels must have static lifetime. Recording never logs or allocates.
inline void MarkStartupTiming(const char* static_label) {
    auto& state = detail::StartupTiming();
    if (state.count == 0U || state.reported || static_label == nullptr) return;
    if (state.count == state.marks.size()) {
        state.overflow = true;
        return;
    }
    auto& mark = state.marks[state.count++];
    mark.timestamp_us = esp_timer_get_time();
    mark.label = static_label;
}

// Call after the first complete frame and DISPLAY_ON have been submitted.
inline void ReportStartupTiming() {
    auto& state = detail::StartupTiming();
    if (state.count == 0U || state.reported) return;
    state.reported = true;
    constexpr char kTag[] = "startup_timing";
    ESP_LOGI(kTag,
             "reset=%d power_on=%u app_main_rtc_api_us=%" PRIu64 " app_main_timer_us=%" PRId64
             " app_main_log_ms=%" PRIu32 " overflow=%u",
             static_cast<int>(state.reset_reason), static_cast<unsigned>(state.reset_reason == ESP_RST_POWERON),
             state.app_main_rtc_api_us, state.marks[0].timestamp_us, state.app_main_log_ms,
             static_cast<unsigned>(state.overflow));
    ESP_LOGI(kTag,
             "app_main_raw_rtc_ticks=%" PRIu64 " slowclk_cal_q19=%" PRIu32 " app_main_raw_rtc_estimate_us=%" PRIu64,
             state.app_main_raw_rtc_ticks, state.slowclk_cal, state.app_main_raw_rtc_estimate_us);
    // ESP Timer starts during application initialization, after bootloader and
    // early PSRAM setup. RTC time persists across software/deep-sleep resets.
    const int64_t begin_us = state.marks[0].timestamp_us;
    int64_t previous_us = begin_us;
    for (size_t index = 1U; index < state.count; ++index) {
        const auto& mark = state.marks[index];
        ESP_LOGI(kTag, "stage=%s elapsed_us=%" PRId64 " delta_us=%" PRId64, mark.label, mark.timestamp_us - begin_us,
                 mark.timestamp_us - previous_us);
        previous_us = mark.timestamp_us;
    }
}
#else
inline void BeginStartupTiming() {}
inline void MarkStartupTiming(const char*) {}
inline void ReportStartupTiming() {}
#endif

}  // namespace micropixel::platform::diagnostics
