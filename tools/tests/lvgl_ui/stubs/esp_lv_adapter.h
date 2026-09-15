// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
inline esp_err_t esp_lv_adapter_lock(int) { return ESP_OK; }
inline void esp_lv_adapter_unlock() {}
inline esp_err_t esp_lv_adapter_request_wake() { return ESP_OK; }
