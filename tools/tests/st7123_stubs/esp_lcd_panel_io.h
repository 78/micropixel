// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>

#include "esp_err.h"
typedef void* esp_lcd_panel_io_handle_t;
esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t, int, void*, size_t);
