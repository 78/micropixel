#ifndef MICROPIXEL_TEST_STUB_ESP_TIMER_H
#define MICROPIXEL_TEST_STUB_ESP_TIMER_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "esp_err.h"

using esp_timer_cb_t = void (*)(void*);

enum esp_timer_dispatch_t {
    ESP_TIMER_TASK = 0,
};

struct esp_timer_create_args_t final {
    esp_timer_cb_t callback{};
    void* arg{};
    esp_timer_dispatch_t dispatch_method{ESP_TIMER_TASK};
    const char* name{};
    bool skip_unhandled_events{};
};

struct MicropixelTestEspTimer final {
    esp_timer_cb_t callback{};
    void* argument{};
    std::atomic<uint64_t> generation{};
    std::atomic<bool> active{};
};

using esp_timer_handle_t = MicropixelTestEspTimer*;

inline esp_err_t esp_timer_create(const esp_timer_create_args_t* arguments, esp_timer_handle_t* timer_out) {
    if (arguments == nullptr || arguments->callback == nullptr || timer_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    auto* timer = new MicropixelTestEspTimer;
    timer->callback = arguments->callback;
    timer->argument = arguments->arg;
    *timer_out = timer;
    return ESP_OK;
}

inline esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us) {
    if (timer == nullptr || timeout_us == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint64_t generation = timer->generation.fetch_add(1U) + 1U;
    timer->active.store(true);
    std::thread([timer, timeout_us, generation] {
        std::this_thread::sleep_for(std::chrono::microseconds(timeout_us));
        if (timer->active.load() && timer->generation.load() == generation) {
            timer->active.store(false);
            timer->callback(timer->argument);
        }
    }).detach();
    return ESP_OK;
}

inline esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    if (timer == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool was_active = timer->active.exchange(false);
    (void)timer->generation.fetch_add(1U);
    return was_active ? ESP_OK : ESP_ERR_INVALID_STATE;
}

inline esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer, uint32_t) {
    if (timer == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    timer->active.store(false);
    (void)timer->generation.fetch_add(1U);
    return ESP_OK;
}

inline esp_err_t esp_timer_delete(esp_timer_handle_t timer) {
    if (timer == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    timer->active.store(false);
    (void)timer->generation.fetch_add(1U);
    // Detached test callbacks can still hold this address briefly. Keep the
    // tiny test object alive until process exit rather than risking a UAF.
    return ESP_OK;
}

#endif
