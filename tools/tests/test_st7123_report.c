// SPDX-License-Identifier: Apache-2.0
#include <assert.h>

// Exercise the actual kevincoooool/esp_lcd_touch_st7123 callbacks in
// esp_lcd_touch_get_data order. test_firmware_host.sh passes the fetched
// component source as ST7123_SOURCE.
#ifndef ST7123_SOURCE
#error "Define ST7123_SOURCE as the path of esp_lcd_touch_st7123.c"
#endif
#include ST7123_SOURCE

static uint8_t advanced_info;
static uint8_t reports[70];
static unsigned report_reads;

esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t io, int command, void* data, size_t length) {
    (void)io;
    if (command == 0x10) {
        *(uint8_t*)data = advanced_info;
    } else if (command == MAX_TOUCHES_REG) {
        *(uint8_t*)data = 10;
    } else if (command == REPORT_COORD_0_REG) {
        assert(length == sizeof(reports));
        memcpy(data, reports, length);
        ++report_reads;
    } else {
        memset(data, 0, length);
    }
    return ESP_OK;
}

int main(void) {
    esp_lcd_touch_t touch = {0};
    touch.config.int_gpio_num = 23;
    advanced_info = 0x08;  // Coordinate report.
    reports[2 * 7] = 0x80;
    reports[2 * 7 + 1] = 100;
    reports[2 * 7 + 3] = 200;
    reports[7 * 7] = 0x80;
    reports[7 * 7 + 1] = 150;
    reports[7 * 7 + 3] = 250;
    assert(read_data(&touch) == ESP_OK);
    uint16_t x[5], y[5], strength[5];
    uint8_t count = 0;
    uint8_t ids[5];
    memset(ids, 0xff, sizeof(ids));
    assert(get_xy(&touch, x, y, strength, &count, 5));
    assert(count == 2 && touch.data.points == 0);
    assert(get_track_id(&touch, ids, count) == ESP_OK);
    assert(ids[0] == 2 && ids[1] == 7 && ids[2] == 0xff);
    assert(x[0] == 100 && y[0] == 200 && x[1] == 150 && y[1] == 250);

    // Lift reports must clear the held state, including a status-only frame.
    memset(reports, 0, sizeof(reports));
    assert(read_data(&touch) == ESP_OK);
    assert(!get_xy(&touch, x, y, strength, &count, 5) && count == 0);
    touch.data.points = 1;
    advanced_info = 0;
    const unsigned previous_reads = report_reads;
    assert(read_data(&touch) == ESP_OK);
    assert(report_reads == previous_reads);
    assert(!get_xy(&touch, x, y, strength, &count, 5) && count == 0);
    puts("ST7123 callback ordering and release tests passed");
    return 0;
}
