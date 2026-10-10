// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/m5stack-tab5/rtc_clock.hpp"

#include <sys/time.h>

#include <cstdio>
#include <ctime>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "platform/boards/m5stack-tab5/board_config.hpp"

namespace micropixel::platform::m5stack_tab5 {
namespace {

constexpr char kTag[] = "tab5_rtc";
// Writing the calendar costs two control-register writes plus the burst; once a
// minute keeps the RTC within a second of a clock SNTP corrected.
constexpr uint64_t kRefreshIntervalUs = 60U * 1000U * 1000U;
// Below this the two clocks agree and nothing is written.
constexpr int64_t kWriteBackThresholdSeconds = 2;
constexpr size_t kClockTextSize = 32U;

void FormatClock(const drivers::rx8130::Time& time, char* out, size_t size) {
    (void)snprintf(out, size, "%04u-%02u-%02u %02u:%02u:%02u UTC", static_cast<unsigned>(time.year),
                   static_cast<unsigned>(time.month), static_cast<unsigned>(time.day), static_cast<unsigned>(time.hour),
                   static_cast<unsigned>(time.minute), static_cast<unsigned>(time.second));
}

}  // namespace

RtcClock::~RtcClock() {
    if (refresh_timer_ != nullptr) {
        (void)esp_timer_stop_blocking(refresh_timer_, portMAX_DELAY);
        (void)esp_timer_delete(refresh_timer_);
    }
}

void RtcClock::Initialize(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor) {
    executor_ = &executor;
    const esp_err_t bind_status = rtc_.Bind(bus, {board::kRtcAddress});
    if (bind_status != ESP_OK) {
        ESP_LOGW(kTag, "RTC bind failed: %s", esp_err_to_name(bind_status));
    }
    // The first sync runs inline so the clock is already set when the UI and
    // the network stack come up; later refreshes post to the shared I2C worker.
    if (executor.Invoke(buses::I2cExecutor::Priority::kLow, RefreshEntry, this) != ESP_OK) {
        ESP_LOGW(kTag, "initial RTC sync could not run");
    }
    esp_timer_create_args_t arguments{};
    arguments.callback = RefreshTimer;
    arguments.arg = this;
    arguments.dispatch_method = ESP_TIMER_TASK;
    arguments.name = "tab5_rtc";
    arguments.skip_unhandled_events = true;
    if (esp_timer_create(&arguments, &refresh_timer_) != ESP_OK ||
        esp_timer_start_periodic(refresh_timer_, kRefreshIntervalUs) != ESP_OK) {
        ESP_LOGW(kTag, "periodic RTC refresh unavailable");
    }
}

void RtcClock::SyncOnWorker() {
    if (!configured_) {
        if (rtc_.Configure() != ESP_OK) {
            return;  // the driver logs the failure and retries with backoff
        }
        configured_ = true;
        if (rtc_.VoltageLossDetected()) {
            ESP_LOGW(kTag,
                     "the RTC calendar is untrusted after a power-down; it is rewritten once the clock is trustworthy");
        } else {
            RestoreSystemClock();
        }
    }
    PersistSystemClock();
}

void RtcClock::RestoreSystemClock() {
    drivers::rx8130::Fields fields{};
    if (rtc_.ReadCalendar(fields) != ESP_OK) {
        return;
    }
    drivers::rx8130::Time rtc_time{};
    if (!drivers::rx8130::Decode(fields, rtc_time)) {
        ESP_LOGI(kTag, "the RTC calendar is not BCD noise: %02x %02x %02x %02x %02x %02x %02x",
                 static_cast<unsigned>(fields.second), static_cast<unsigned>(fields.minute),
                 static_cast<unsigned>(fields.hour), static_cast<unsigned>(fields.weekday),
                 static_cast<unsigned>(fields.day), static_cast<unsigned>(fields.month),
                 static_cast<unsigned>(fields.year));
        return;
    }
    if (!drivers::rx8130::IsPlausible(rtc_time)) {
        char pending[kClockTextSize] = {};
        FormatClock(rtc_time, pending, sizeof(pending));
        ESP_LOGI(kTag, "the RTC holds no usable time yet (reads %s)", pending);
        return;
    }
    const timeval value{
        .tv_sec = static_cast<time_t>(drivers::rx8130::ToEpochSeconds(rtc_time)),
        .tv_usec = 0,
    };
    if (settimeofday(&value, nullptr) != 0) {
        ESP_LOGW(kTag, "could not set the system clock from the RTC");
        return;
    }
    char text[kClockTextSize] = {};
    FormatClock(rtc_time, text, sizeof(text));
    ESP_LOGI(kTag, "system clock restored from RTC: %s", text);
}

void RtcClock::PersistSystemClock() {
    drivers::rx8130::Time system_time{};
    if (!drivers::rx8130::FromEpochSeconds(static_cast<int64_t>(time(nullptr)), system_time) ||
        !drivers::rx8130::IsPlausible(system_time)) {
        return;  // the system clock is not trustworthy yet, so it must not win
    }
    drivers::rx8130::Fields fields{};
    drivers::rx8130::Time rtc_time{};
    const bool rtc_valid = rtc_.ReadCalendar(fields) == ESP_OK && drivers::rx8130::Decode(fields, rtc_time) &&
                           drivers::rx8130::IsPlausible(rtc_time);
    if (rtc_valid) {
        const int64_t difference =
            drivers::rx8130::ToEpochSeconds(system_time) - drivers::rx8130::ToEpochSeconds(rtc_time);
        if (difference <= kWriteBackThresholdSeconds && difference >= -kWriteBackThresholdSeconds) {
            char text[kClockTextSize] = {};
            FormatClock(rtc_time, text, sizeof(text));
            ESP_LOGD(kTag, "RTC calendar matches the system clock (%+lld s): %s", static_cast<long long>(difference),
                     text);
            (void)rtc_.ClearVoltageLoss();
            return;
        }
    }
    drivers::rx8130::Fields encoded{};
    drivers::rx8130::Encode(system_time, encoded);
    if (rtc_.WriteCalendar(encoded) != ESP_OK) {
        return;
    }
    char text[kClockTextSize] = {};
    FormatClock(system_time, text, sizeof(text));
    ESP_LOGI(kTag, "RTC updated from the system clock: %s", text);
    (void)rtc_.ClearVoltageLoss();
}

void RtcClock::RefreshTimer(void* context) {
    auto* clock = static_cast<RtcClock*>(context);
    if (clock == nullptr || clock->executor_ == nullptr ||
        clock->refresh_pending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!clock->executor_->Post(buses::I2cExecutor::Priority::kLow, RefreshEntry, clock)) {
        clock->refresh_pending_.store(false, std::memory_order_release);
    }
}

esp_err_t RtcClock::RefreshEntry(void* context) {
    auto* clock = static_cast<RtcClock*>(context);
    if (clock == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    clock->refresh_pending_.store(false, std::memory_order_release);
    clock->SyncOnWorker();
    return ESP_OK;
}

}  // namespace micropixel::platform::m5stack_tab5
