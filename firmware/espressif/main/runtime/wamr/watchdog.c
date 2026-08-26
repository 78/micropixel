#include "runtime/wamr/watchdog.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    wasm_module_inst_t module_inst;
    uint32_t timeout_ms;
    int64_t deadline_us;
    pthread_mutex_t lock;
    esp_timer_handle_t timer;
    bool completed;
    bool timed_out;
    bool paused;
    bool initialized;
} guest_watchdog_t;

static const char* TAG = "micropixel_watchdog";
static _Thread_local guest_watchdog_t* active_watchdog;

static bool arm_for_deadline_locked(guest_watchdog_t* watchdog) {
    uint64_t deadline_us = (uint64_t)watchdog->deadline_us;
    esp_err_t err = esp_timer_restart_at(watchdog->timer, 0, deadline_us);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_timer_start_once_at(watchdog->timer, deadline_us);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "unable to arm guest watchdog: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void terminate_guest(guest_watchdog_t* watchdog) {
    ESP_LOGW(TAG, "guest exceeded watchdog quota: %" PRIu32 " ms", watchdog->timeout_ms);
    wasm_runtime_terminate(watchdog->module_inst);
}

static void guest_watchdog_callback(void* arg) {
    guest_watchdog_t* watchdog = arg;
    bool terminate = false;

    pthread_mutex_lock(&watchdog->lock);
    if (!watchdog->completed && !watchdog->timed_out && !watchdog->paused) {
        int64_t now_us = esp_timer_get_time();
        if (now_us >= watchdog->deadline_us) {
            watchdog->timed_out = true;
            terminate = true;
        } else if (!esp_timer_is_active(watchdog->timer) && !arm_for_deadline_locked(watchdog)) {
            watchdog->timed_out = true;
            terminate = true;
        }
    }
    pthread_mutex_unlock(&watchdog->lock);

    if (terminate) {
        terminate_guest(watchdog);
    }
}

static bool guest_watchdog_start(guest_watchdog_t* watchdog, wasm_module_inst_t module_inst, uint32_t timeout_ms) {
    memset(watchdog, 0, sizeof(*watchdog));
    watchdog->module_inst = module_inst;
    watchdog->timeout_ms = timeout_ms;

    if (pthread_mutex_init(&watchdog->lock, NULL) != 0) {
        return false;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = guest_watchdog_callback,
        .arg = watchdog,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "guest_watchdog",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &watchdog->timer) != ESP_OK) {
        pthread_mutex_destroy(&watchdog->lock);
        return false;
    }
    watchdog->initialized = true;

    watchdog->deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    if (!arm_for_deadline_locked(watchdog)) {
        esp_timer_delete(watchdog->timer);
        pthread_mutex_destroy(&watchdog->lock);
        watchdog->initialized = false;
        return false;
    }
    return true;
}

static bool guest_watchdog_finish(guest_watchdog_t* watchdog) {
    if (!watchdog->initialized) {
        return false;
    }

    pthread_mutex_lock(&watchdog->lock);
    watchdog->completed = true;
    pthread_mutex_unlock(&watchdog->lock);

    esp_err_t stop_err = esp_timer_stop_blocking(watchdog->timer, portMAX_DELAY);
    if (stop_err != ESP_OK) {
        ESP_LOGE(TAG, "unable to stop guest watchdog safely: %s", esp_err_to_name(stop_err));
    }

    bool timed_out = watchdog->timed_out;
    esp_err_t delete_err = esp_timer_delete(watchdog->timer);
    if (delete_err != ESP_OK) {
        ESP_LOGE(TAG, "unable to delete guest watchdog: %s", esp_err_to_name(delete_err));
    }
    pthread_mutex_destroy(&watchdog->lock);
    watchdog->initialized = false;
    return timed_out;
}

void micropixel_watchdog_checkpoint(void) {
    guest_watchdog_t* watchdog = active_watchdog;
    if (watchdog == NULL || !watchdog->initialized) {
        return;
    }

    bool terminate = false;
    pthread_mutex_lock(&watchdog->lock);
    if (!watchdog->completed && !watchdog->timed_out) {
        watchdog->paused = false;
        watchdog->deadline_us = esp_timer_get_time() + (int64_t)watchdog->timeout_ms * 1000;
        if (!arm_for_deadline_locked(watchdog)) {
            watchdog->timed_out = true;
            terminate = true;
        }
    }
    pthread_mutex_unlock(&watchdog->lock);

    if (terminate) {
        terminate_guest(watchdog);
    }
}

void micropixel_watchdog_pause(void) {
    guest_watchdog_t* watchdog = active_watchdog;
    if (watchdog == NULL || !watchdog->initialized) {
        return;
    }

    pthread_mutex_lock(&watchdog->lock);
    if (!watchdog->completed && !watchdog->timed_out) {
        watchdog->paused = true;
        esp_err_t err = esp_timer_stop(watchdog->timer);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "unable to pause guest watchdog: %s", esp_err_to_name(err));
        }
    }
    pthread_mutex_unlock(&watchdog->lock);
}

void micropixel_watchdog_resume(void) { micropixel_watchdog_checkpoint(); }

bool micropixel_call_with_watchdog(wasm_module_inst_t module_inst, wasm_exec_env_t exec_env,
                                   wasm_function_inst_t function, uint32_t argc, uint32_t argv[], uint32_t timeout_ms,
                                   bool* timed_out_out) {
    guest_watchdog_t watchdog;

    if (timed_out_out != NULL) {
        *timed_out_out = false;
    }
    if (!guest_watchdog_start(&watchdog, module_inst, timeout_ms)) {
        ESP_LOGE(TAG, "unable to create guest watchdog");
        return false;
    }

    guest_watchdog_t* previous_watchdog = active_watchdog;
    active_watchdog = &watchdog;
    bool call_succeeded = wasm_runtime_call_wasm(exec_env, function, argc, argv);
    active_watchdog = previous_watchdog;
    bool timed_out = guest_watchdog_finish(&watchdog);
    if (timed_out_out != NULL) {
        *timed_out_out = timed_out;
    }
    return call_succeeded;
}
