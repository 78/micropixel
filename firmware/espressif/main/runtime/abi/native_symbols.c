#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "abi/micropixel_abi.h"
#include "esp_log.h"
#include "runtime/abi/abi_bridge.h"
#include "runtime/wamr/watchdog.h"

static const char* TAG = "micropixel_abi";

static uint32_t host_abi_version(wasm_exec_env_t exec_env) {
    (void)exec_env;
    micropixel_watchdog_checkpoint();
    return MICROPIXEL_ABI_VERSION;
}

static int32_t host_log_write(wasm_exec_env_t exec_env, uint32_t level, const uint8_t* bytes, uint32_t length) {
    (void)exec_env;
    micropixel_watchdog_checkpoint();
    if ((bytes == NULL && length != 0U) || length > MICROPIXEL_ABI_MAX_LOG_BYTES) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }

    const char* message = bytes != NULL ? (const char*)bytes : "";
    switch (level) {
        case MICROPIXEL_LOG_DEBUG:
            ESP_LOGD(TAG, "guest: %.*s", (int)length, message);
            break;
        case MICROPIXEL_LOG_INFO:
            ESP_LOGI(TAG, "guest: %.*s", (int)length, message);
            break;
        case MICROPIXEL_LOG_WARNING:
            ESP_LOGW(TAG, "guest: %.*s", (int)length, message);
            break;
        case MICROPIXEL_LOG_ERROR:
            ESP_LOGE(TAG, "guest: %.*s", (int)length, message);
            break;
        default:
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    return MICROPIXEL_STATUS_OK;
}

static NativeSymbol micropixel_symbols[] = {
    {"abi_version", host_abi_version, "()i", NULL},
    {"log_write", host_log_write, "(i*~)i", NULL},
    {"event_wait", micropixel_runtime_event_wait, "(*~I)i", NULL},
    {"clock_now", micropixel_runtime_clock_now, "()I", NULL},
    {"service_open", micropixel_runtime_service_open, "(ii*~)i", NULL},
    {"service_call", micropixel_runtime_service_call, "(ii*~*~*)i", NULL},
    {"service_submit", micropixel_runtime_service_submit, "(ii*~)i", NULL},
};

bool micropixel_register_native_apis(void) {
    if (!wasm_runtime_register_natives("micropixel", micropixel_symbols,
                                       sizeof(micropixel_symbols) / sizeof(micropixel_symbols[0]))) {
        ESP_LOGE(TAG, "failed to register MicroPixel ABI v1");
        return false;
    }
    return true;
}
