#pragma once
#include "nvs.h"
esp_err_t nvs_get_i32(nvs_handle_t, const char*, int32_t*);
esp_err_t nvs_set_i32(nvs_handle_t, const char*, int32_t);
