#include "runtime/wamr/diagnostics.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WAMR_TASK_STACK_SIZE (16U * 1024U)

static const char* TAG = "micropixel_diag";

static void log_native_stack_profile(const char* label, size_t configured, size_t minimum_free) {
    if (minimum_free == SIZE_MAX) {
        ESP_LOGW(TAG, "%s native stack: no sample", label);
        return;
    }

    size_t maximum_used = configured > minimum_free ? configured - minimum_free : 0;
    ESP_LOGI(TAG, "%s native stack: configured=%zu B, max-used=%zu B, minimum-free=%zu B, utilization=%zu%%", label,
             configured, maximum_used, minimum_free, configured > 0 ? maximum_used * 100U / configured : 0);
}

void micropixel_log_heap_state(const char* label) {
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    ESP_LOGI(TAG, "%s internal SRAM heap: total=%zu free=%zu boot-minimum=%zu largest=%zu", label,
             heap_caps_get_total_size(internal_caps), heap_caps_get_free_size(internal_caps),
             heap_caps_get_minimum_free_size(internal_caps), heap_caps_get_largest_free_block(internal_caps));
    ESP_LOGI(TAG, "%s PSRAM heap: total=%zu free=%zu boot-minimum=%zu largest=%zu", label,
             heap_caps_get_total_size(psram_caps), heap_caps_get_free_size(psram_caps),
             heap_caps_get_minimum_free_size(psram_caps), heap_caps_get_largest_free_block(psram_caps));
}

void micropixel_log_stack_profiles(void) {
    size_t host_minimum_free = (size_t)uxTaskGetStackHighWaterMark2(NULL);
    log_native_stack_profile("WAMR Host pthread", WAMR_TASK_STACK_SIZE, host_minimum_free);
}
