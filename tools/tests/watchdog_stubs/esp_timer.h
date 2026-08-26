#ifndef MICROPIXEL_TEST_WATCHDOG_STUB_ESP_TIMER_H
#define MICROPIXEL_TEST_WATCHDOG_STUB_ESP_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct test_esp_timer* esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void* arg);

typedef enum {
    ESP_TIMER_TASK,
} esp_timer_dispatch_t;

typedef struct {
    esp_timer_cb_t callback;
    void* arg;
    esp_timer_dispatch_t dispatch_method;
    const char* name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;

esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* timer_out);
esp_err_t esp_timer_start_once_at(esp_timer_handle_t timer, uint64_t alarm_us);
esp_err_t esp_timer_restart_at(esp_timer_handle_t timer, uint64_t period_us, uint64_t first_alarm_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer, uint32_t timeout_ticks);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
int64_t esp_timer_get_time(void);
bool esp_timer_is_active(esp_timer_handle_t timer);

#endif
