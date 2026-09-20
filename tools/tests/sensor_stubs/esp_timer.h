// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <string_view>

#include "esp_err.h"

using esp_timer_cb_t = void (*)(void*);
enum esp_timer_dispatch_t { ESP_TIMER_TASK };
struct esp_timer_create_args_t {
    esp_timer_cb_t callback{};
    void* arg{};
    esp_timer_dispatch_t dispatch_method{};
    const char* name{};
    bool skip_unhandled_events{};
};
struct TestSensorTimer {
    esp_timer_create_args_t arguments{};
    uint64_t interval{};
    bool allocated{};
    bool active{};
};
using esp_timer_handle_t = TestSensorTimer*;
inline std::array<TestSensorTimer, 16> sensor_timers{};
inline bool fail_timer_create{};
inline bool fail_timer_start{};
inline int64_t sensor_time_us{12345};
inline int64_t esp_timer_get_time() { return sensor_time_us; }
inline esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out) {
    if (fail_timer_create) return ESP_ERR_NO_MEM;
    for (auto& timer : sensor_timers) {
        if (!timer.allocated) {
            timer = {.arguments = *args, .allocated = true};
            *out = &timer;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}
inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t interval) {
    if (fail_timer_start || interval == 0) return ESP_FAIL;
    timer->interval = interval;
    timer->active = true;
    return ESP_OK;
}
inline esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer, uint32_t) {
    timer->active = false;
    return ESP_OK;
}
inline esp_err_t esp_timer_delete(esp_timer_handle_t timer) {
    *timer = {};
    return ESP_OK;
}
inline void FireSensorTimer(std::string_view name) {
    for (auto& timer : sensor_timers) {
        if (timer.allocated && timer.active && timer.arguments.name == name) {
            timer.arguments.callback(timer.arguments.arg);
            return;
        }
    }
    assert(false && "expected an active sensor timer");
}
