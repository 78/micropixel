/*
 * SPDX-FileCopyrightText: 2024 Seeed Technology Co., Ltd
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Panel initialization data only, copied from
 * Seeed-Studio/reTerminal-D1001 components/esp_lcd_jd9365_8
 * (vendor_specific_init_default). The driver code is the upstream
 * espressif/esp_lcd_jd9365 component.
 */
#include "platform/boards/reterminal-d1001/display/jd9365_init_data.h"

const jd9365_lcd_init_cmd_t d1001_lcd_jd9365_init_cmds[] = {
    // {cmd, { data }, data_size, delay_ms}
    {0x11, (uint8_t[]){0x00}, 1, 500},  // Sleep out
    {0xE0, (uint8_t[]){0x00}, 1, 0},
    {0xE1, (uint8_t[]){0x93}, 1, 0},
    {0xE2, (uint8_t[]){0x65}, 1, 0},
    {0xE3, (uint8_t[]){0xF8}, 1, 0},
    {0x80, (uint8_t[]){0x01}, 1, 0},
    {0xE0, (uint8_t[]){0x00}, 1, 0},
    {0x29, (uint8_t[]){0x00}, 1, 50},  // Display on
};

const size_t d1001_lcd_jd9365_init_cmds_count =
    sizeof(d1001_lcd_jd9365_init_cmds) / sizeof(d1001_lcd_jd9365_init_cmds[0]);