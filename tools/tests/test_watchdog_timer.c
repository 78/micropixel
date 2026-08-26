#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_timer.h"
#include "runtime/wamr/watchdog.h"

struct test_esp_timer {
    esp_timer_cb_t callback;
    void* arg;
    uint64_t alarm_us;
    bool active;
};

static int64_t fake_now_us;
static esp_timer_handle_t last_timer;
static unsigned terminate_count;
static unsigned stop_blocking_count;
static void (*wasm_call_hook)(void);

const char* esp_err_to_name(esp_err_t error) {
    (void)error;
    return "test error";
}

esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* timer_out) {
    struct test_esp_timer* timer = calloc(1, sizeof(*timer));
    assert(timer != NULL);
    timer->callback = args->callback;
    timer->arg = args->arg;
    *timer_out = timer;
    last_timer = timer;
    return ESP_OK;
}

esp_err_t esp_timer_start_once_at(esp_timer_handle_t timer, uint64_t alarm_us) {
    if (timer->active) {
        return ESP_ERR_INVALID_STATE;
    }
    timer->alarm_us = alarm_us;
    timer->active = true;
    return ESP_OK;
}

esp_err_t esp_timer_restart_at(esp_timer_handle_t timer, uint64_t period_us, uint64_t first_alarm_us) {
    (void)period_us;
    if (!timer->active) {
        return ESP_ERR_INVALID_STATE;
    }
    timer->alarm_us = first_alarm_us;
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    if (!timer->active) {
        return ESP_ERR_INVALID_STATE;
    }
    timer->active = false;
    return ESP_OK;
}

esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer, uint32_t timeout_ticks) {
    (void)timeout_ticks;
    timer->active = false;
    ++stop_blocking_count;
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer) {
    assert(!timer->active);
    if (last_timer == timer) {
        last_timer = NULL;
    }
    free(timer);
    return ESP_OK;
}

int64_t esp_timer_get_time(void) { return fake_now_us; }

bool esp_timer_is_active(esp_timer_handle_t timer) { return timer->active; }

void wasm_runtime_terminate(wasm_module_inst_t module_inst) {
    (void)module_inst;
    ++terminate_count;
}

bool wasm_runtime_call_wasm(wasm_exec_env_t exec_env, wasm_function_inst_t function, uint32_t argc, uint32_t argv[]) {
    (void)exec_env;
    (void)function;
    (void)argc;
    (void)argv;
    if (wasm_call_hook != NULL) {
        wasm_call_hook();
    }
    return true;
}

static void fire_due_timer(esp_timer_handle_t timer) {
    assert(timer->active);
    assert(fake_now_us >= (int64_t)timer->alarm_us);
    timer->active = false;
    timer->callback(timer->arg);
}

static esp_timer_handle_t active_timer(void) {
    assert(last_timer != NULL);
    return last_timer;
}

static void timeout_hook(void) {
    esp_timer_handle_t timer = active_timer();
    fake_now_us = 1001 * 1000;
    fire_due_timer(timer);
}

static void checkpoint_hook(void) {
    esp_timer_handle_t timer = active_timer();
    fake_now_us = 900 * 1000;
    micropixel_watchdog_checkpoint();

    // Simulate a callback that was dispatched for the old deadline before the restart.
    fake_now_us = 1001 * 1000;
    timer->callback(timer->arg);
}

static void pause_resume_hook(void) {
    esp_timer_handle_t timer = active_timer();
    fake_now_us = 900 * 1000;
    micropixel_watchdog_pause();

    fake_now_us = 1500 * 1000;
    timer->callback(timer->arg);
    assert(terminate_count == 0);

    micropixel_watchdog_resume();
    fake_now_us = 2501 * 1000;
    fire_due_timer(timer);
}

int main(void) {
    bool timed_out = true;

    wasm_call_hook = NULL;
    assert(micropixel_call_with_watchdog(NULL, NULL, NULL, 0, NULL, 1000, &timed_out));
    assert(!timed_out);
    assert(terminate_count == 0);

    fake_now_us = 0;
    wasm_call_hook = timeout_hook;
    assert(micropixel_call_with_watchdog(NULL, NULL, NULL, 0, NULL, 1000, &timed_out));
    assert(timed_out);
    assert(terminate_count == 1);

    fake_now_us = 0;
    terminate_count = 0;
    wasm_call_hook = checkpoint_hook;
    assert(micropixel_call_with_watchdog(NULL, NULL, NULL, 0, NULL, 1000, &timed_out));
    assert(!timed_out);
    assert(terminate_count == 0);

    fake_now_us = 0;
    wasm_call_hook = pause_resume_hook;
    assert(micropixel_call_with_watchdog(NULL, NULL, NULL, 0, NULL, 1000, &timed_out));
    assert(timed_out);
    assert(terminate_count == 1);
    assert(stop_blocking_count == 4);
    return 0;
}
