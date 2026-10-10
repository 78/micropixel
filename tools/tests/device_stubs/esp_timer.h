// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>

#include "esp_err.h"

// Espressif timer stub with an injectable monotonic clock and recorded handles:
// driver backoff windows become deterministic and a board test can fire the
// periodic callback itself instead of waiting for the real interval.
using esp_timer_cb_t = void (*)(void*);

enum esp_timer_dispatch_t { ESP_TIMER_TASK = 0 };

struct esp_timer_create_args_t final {
    esp_timer_cb_t callback{};
    void* arg{};
    esp_timer_dispatch_t dispatch_method{ESP_TIMER_TASK};
    const char* name{};
    bool skip_unhandled_events{};
};

struct FakeEspTimer final {
    esp_timer_cb_t callback{};
    void* argument{};
    uint64_t period_us{};
    bool periodic{};
    bool active{};
};

using esp_timer_handle_t = FakeEspTimer*;

inline int64_t fake_esp_timer_now_us{};
inline FakeEspTimer* fake_esp_timer_last{};
inline int fake_esp_timer_start_failures{};

inline std::array<FakeEspTimer, 4U>& FakeEspTimerPool() {
    static std::array<FakeEspTimer, 4U> pool{};
    return pool;
}

inline std::size_t& FakeEspTimerCount() {
    static std::size_t count{};
    return count;
}

inline int64_t esp_timer_get_time() {
    return fake_esp_timer_now_us;
}

inline esp_err_t esp_timer_create(const esp_timer_create_args_t* arguments, esp_timer_handle_t* timer_out) {
    if (arguments == nullptr || arguments->callback == nullptr || timer_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    std::array<FakeEspTimer, 4U>& pool = FakeEspTimerPool();
    if (FakeEspTimerCount() >= pool.size()) {
        return ESP_ERR_NO_MEM;
    }
    FakeEspTimer& timer = pool[FakeEspTimerCount()++];
    timer = {};
    timer.callback = arguments->callback;
    timer.argument = arguments->arg;
    *timer_out = &timer;
    fake_esp_timer_last = &timer;
    return ESP_OK;
}

inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us) {
    if (timer == nullptr || period_us == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fake_esp_timer_start_failures > 0) {
        --fake_esp_timer_start_failures;
        return ESP_FAIL;
    }
    timer->period_us = period_us;
    timer->periodic = true;
    timer->active = true;
    return ESP_OK;
}

inline esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us) {
    if (timer == nullptr || timeout_us == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    timer->period_us = timeout_us;
    timer->periodic = false;
    timer->active = true;
    return ESP_OK;
}

inline esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    if (timer == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool was_active = timer->active;
    timer->active = false;
    return was_active ? ESP_OK : ESP_ERR_INVALID_STATE;
}

inline esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer, uint32_t timeout_ticks) {
    (void)timeout_ticks;
    if (timer == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    timer->active = false;
    return ESP_OK;
}

inline esp_err_t esp_timer_delete(esp_timer_handle_t timer) {
    if (timer == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // The pool outlives every timer, so nothing needs to be freed here.
    timer->active = false;
    timer->callback = nullptr;
    return ESP_OK;
}

// Fire one periodic tick exactly as the Host would receive it.
inline void FakeFireEspTimer(esp_timer_handle_t timer) {
    if (timer != nullptr && timer->active && timer->callback != nullptr) {
        timer->callback(timer->argument);
    }
}

// Hand the pool back before the next scenario builds its own timers.
inline void FakeResetEspTimers() {
    FakeEspTimerPool() = {};
    FakeEspTimerCount() = 0U;
    fake_esp_timer_last = nullptr;
    fake_esp_timer_start_failures = 0;
    fake_esp_timer_now_us = 0;
}
