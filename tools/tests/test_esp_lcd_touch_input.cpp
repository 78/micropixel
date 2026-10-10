// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdio>
#include <vector>

#include "platform/input/esp_lcd_touch_input.hpp"

namespace {
int interrupt_level = 1;
esp_err_t read_status = ESP_ERR_INVALID_RESPONSE;
uint8_t report_count{};
unsigned reads{};
void (*interrupt_callback)(esp_lcd_touch_handle_t){};
std::vector<micropixel::device::TouchPhase> phases;
}  // namespace

int gpio_get_level(int pin) {
    assert(pin == 23);
    return interrupt_level;
}
esp_err_t esp_lcd_touch_register_interrupt_callback(esp_lcd_touch_handle_t, void (*callback)(esp_lcd_touch_handle_t)) {
    interrupt_callback = callback;
    return ESP_OK;
}
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t) {
    ++reads;
    return read_status;
}
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t, esp_lcd_touch_point_data_t* points, uint8_t* count, uint8_t) {
    *count = report_count;
    points[0] = {.track_id = 2U, .x = 100U, .y = 200U};
    return ESP_OK;
}

int main() {
    using micropixel::device::TouchPhase;
    using micropixel::platform::input::EspLcdTouchInput;
    micropixel::platform::buses::I2cExecutor executor;
    esp_lcd_touch_t touch;
    touch.config.int_gpio_num = 23;
    EspLcdTouchInput input(480, 800, 5);
    assert(input.Initialize(&touch, executor, true) == ESP_OK);
    input.BindTouchSink(
        [](void*, const micropixel::device::TouchSample& sample) {
            phases.push_back(sample.phase);
            return true;
        },
        nullptr);
    assert(input.Start(reinterpret_cast<lv_display_t*>(1U)) == ESP_OK);
    assert(interrupt_callback != nullptr);
    const unsigned prime_reads = reads;
    FireSensorTimer("touch_poll");
    executor.Drain();
    assert(reads == prime_reads);  // No I2C read while INT is inactive.
    interrupt_level = 0;
    report_count = 1U;
    read_status = ESP_OK;
    executor.reject_post = true;
    interrupt_callback(&touch);  // Queue full; level polling must recover.
    executor.reject_post = false;
    FireSensorTimer("touch_poll");
    executor.Drain();
    assert(phases.size() == 1U && phases.back() == TouchPhase::kDown);
    FireSensorTimer("touch_poll");  // Still low, with no new edge.
    executor.Drain();
    assert(phases.back() == TouchPhase::kMove);
    sensor_time_us += 300000U;
    read_status = ESP_ERR_INVALID_RESPONSE;
    FireSensorTimer("touch_poll");
    executor.Drain();
    assert(phases.size() == 2U);  // A quiet held finger must not be released.
    read_status = ESP_FAIL;
    FireSensorTimer("touch_poll");
    executor.Drain();
    assert(phases.back() == TouchPhase::kCancel);
    read_status = ESP_OK;
    FireSensorTimer("touch_poll");
    executor.Drain();
    assert(phases.back() == TouchPhase::kDown);
    report_count = 0U;
    FireSensorTimer("touch_poll");
    executor.Drain();
    assert(phases.back() == TouchPhase::kUp);
    std::puts("INT-level touch input tests passed");
}
