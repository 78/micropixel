// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>

#include "esp_lcd_jd9365.h"

#ifdef __cplusplus
extern "C" {
#endif

// Vendor panel initialization for the Seeed reTerminal D1001 8-inch 800x1280
// module, copied from Seeed-Studio/reTerminal-D1001
// components/esp_lcd_jd9365_8 (Apache-2.0) and used with the upstream
// espressif/esp_lcd_jd9365 driver.
extern const jd9365_lcd_init_cmd_t d1001_lcd_jd9365_init_cmds[];
extern const size_t d1001_lcd_jd9365_init_cmds_count;

#ifdef __cplusplus
}
#endif