#include "runtime/wamr/watchdog.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    wasm_module_inst_t module_inst;
    uint32_t timeout_ms;
    struct timespec deadline;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    pthread_t thread;
    bool completed;
    bool timed_out;
    bool paused;
    bool initialized;
    bool started;
} guest_watchdog_t;

static const char* TAG = "micropixel_watchdog";
static size_t watchdog_stack_min_free = SIZE_MAX;
static _Thread_local guest_watchdog_t* active_watchdog;

static void record_current_stack_min(void) {
    size_t sample = (size_t)uxTaskGetStackHighWaterMark2(NULL);
    size_t current = __atomic_load_n(&watchdog_stack_min_free, __ATOMIC_RELAXED);

    while (sample < current && !__atomic_compare_exchange_n(&watchdog_stack_min_free, &current, sample, true,
                                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

static void add_milliseconds(struct timespec* time, uint32_t milliseconds) {
    time->tv_sec += milliseconds / 1000U;
    time->tv_nsec += (long)(milliseconds % 1000U) * 1000000L;
    if (time->tv_nsec >= 1000000000L) {
        ++time->tv_sec;
        time->tv_nsec -= 1000000000L;
    }
}

static void* guest_watchdog_thread(void* arg) {
    guest_watchdog_t* watchdog = arg;
    bool terminate = false;

    pthread_mutex_lock(&watchdog->lock);
    while (!watchdog->completed) {
        if (watchdog->paused) {
            int rc = pthread_cond_wait(&watchdog->condition, &watchdog->lock);
            if (rc != 0) {
                ESP_LOGE(TAG, "guest watchdog pause wait failed: %d", rc);
                watchdog->timed_out = true;
                terminate = true;
                break;
            }
            continue;
        }

        int rc = pthread_cond_timedwait(&watchdog->condition, &watchdog->lock, &watchdog->deadline);
        if (rc == ETIMEDOUT && !watchdog->completed && !watchdog->paused) {
            watchdog->timed_out = true;
            terminate = true;
            break;
        }
        if (rc != 0) {
            ESP_LOGE(TAG, "guest watchdog wait failed: %d", rc);
            watchdog->timed_out = true;
            terminate = true;
            break;
        }
    }
    pthread_mutex_unlock(&watchdog->lock);

    if (terminate) {
        ESP_LOGW(TAG, "guest exceeded watchdog quota: %" PRIu32 " ms", watchdog->timeout_ms);
        wasm_runtime_terminate(watchdog->module_inst);
    }
    record_current_stack_min();
    return NULL;
}

static bool reset_deadline(guest_watchdog_t* watchdog) {
    if (clock_gettime(CLOCK_REALTIME, &watchdog->deadline) != 0) {
        return false;
    }
    add_milliseconds(&watchdog->deadline, watchdog->timeout_ms);
    return true;
}

static bool guest_watchdog_start(guest_watchdog_t* watchdog, wasm_module_inst_t module_inst, uint32_t timeout_ms) {
    memset(watchdog, 0, sizeof(*watchdog));
    watchdog->module_inst = module_inst;
    watchdog->timeout_ms = timeout_ms;

    if (pthread_mutex_init(&watchdog->lock, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&watchdog->condition, NULL) != 0) {
        pthread_mutex_destroy(&watchdog->lock);
        return false;
    }
    watchdog->initialized = true;

    if (!reset_deadline(watchdog)) {
        pthread_cond_destroy(&watchdog->condition);
        pthread_mutex_destroy(&watchdog->lock);
        watchdog->initialized = false;
        return false;
    }

    if (pthread_create(&watchdog->thread, NULL, guest_watchdog_thread, watchdog) != 0) {
        pthread_cond_destroy(&watchdog->condition);
        pthread_mutex_destroy(&watchdog->lock);
        watchdog->initialized = false;
        return false;
    }
    watchdog->started = true;
    return true;
}

static bool guest_watchdog_finish(guest_watchdog_t* watchdog) {
    if (!watchdog->initialized) {
        return false;
    }

    if (watchdog->started) {
        pthread_mutex_lock(&watchdog->lock);
        watchdog->completed = true;
        pthread_cond_signal(&watchdog->condition);
        pthread_mutex_unlock(&watchdog->lock);
        pthread_join(watchdog->thread, NULL);
        watchdog->started = false;
    }

    bool timed_out = watchdog->timed_out;
    pthread_cond_destroy(&watchdog->condition);
    pthread_mutex_destroy(&watchdog->lock);
    watchdog->initialized = false;
    return timed_out;
}

void micropixel_watchdog_checkpoint(void) {
    guest_watchdog_t* watchdog = active_watchdog;
    if (watchdog == NULL || !watchdog->initialized) {
        return;
    }

    pthread_mutex_lock(&watchdog->lock);
    if (!watchdog->completed && !watchdog->timed_out) {
        if (!reset_deadline(watchdog)) {
            watchdog->timed_out = true;
        }
        watchdog->paused = false;
        pthread_cond_signal(&watchdog->condition);
    }
    pthread_mutex_unlock(&watchdog->lock);
}

void micropixel_watchdog_pause(void) {
    guest_watchdog_t* watchdog = active_watchdog;
    if (watchdog == NULL || !watchdog->initialized) {
        return;
    }

    pthread_mutex_lock(&watchdog->lock);
    if (!watchdog->completed && !watchdog->timed_out) {
        watchdog->paused = true;
        pthread_cond_signal(&watchdog->condition);
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

size_t micropixel_watchdog_stack_min_free(void) { return __atomic_load_n(&watchdog_stack_min_free, __ATOMIC_RELAXED); }
