// SPDX-License-Identifier: Apache-2.0
#include "platform/input/ft6336_report.hpp"

#include <cstddef>
#include <cstdint>

namespace micropixel::platform::input {
namespace {

constexpr uint8_t kStatusRegister = 0x02U;
constexpr size_t kSlotCount = 2U;
constexpr size_t kBytesPerSlot = 6U;
constexpr uint8_t kNoTrack = 0x0fU;
constexpr uint8_t kDown = 0U;
constexpr uint8_t kContact = 2U;
static_assert(CONFIG_ESP_LCD_TOUCH_MAX_POINTS >= kSlotCount);

}  // namespace

esp_err_t ReadFt6336Report(esp_lcd_touch_handle_t touch) {
    if (touch == nullptr || touch->io == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // Read TD_STATUS and both slots in one transaction. Reading a count first
    // and coordinates later can combine different controller scans. The count
    // is the number of active contacts, not the last occupied register slot:
    // slot 0 can contain Up while slot 1 still contains Contact.
    uint8_t report[1U + kSlotCount * kBytesPerSlot]{};
    const esp_err_t status = esp_lcd_panel_io_rx_param(touch->io, kStatusRegister, report, sizeof(report));
    if (status != ESP_OK) {
        return status;
    }
    const uint8_t expected_count = report[0] & 0x0fU;
    if (expected_count > kSlotCount) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_lcd_touch_point_data_t points[kSlotCount]{};
    uint8_t count = 0U;
    // A zero count is an authoritative release; unused slots may be stale.
    if (expected_count != 0U) {
        for (size_t slot = 0U; slot < kSlotCount; ++slot) {
            const uint8_t* data = &report[1U + slot * kBytesPerSlot];
            const uint8_t event = data[0] >> 6U;
            const uint8_t id = data[2] >> 4U;
            if (id == kNoTrack || (event != kDown && event != kContact)) {
                continue;
            }
            const uint16_t x = (static_cast<uint16_t>(data[0] & 0x0fU) << 8U) | data[1];
            const uint16_t y = (static_cast<uint16_t>(data[2] & 0x0fU) << 8U) | data[3];
            if (x > touch->config.x_max || y > touch->config.y_max || (count != 0U && points[0].track_id == id)) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            points[count++] = {.track_id = id, .x = x, .y = y, .strength = 0U};
        }
    }
    if (count != expected_count) {
        // Preserve the preceding tracks on an inconsistent scan. The input
        // adapter skips ESP_ERR_INVALID_RESPONSE without generating Up/Cancel.
        return ESP_ERR_INVALID_RESPONSE;
    }

    portENTER_CRITICAL(&touch->data.lock);
    touch->data.points = count;
    for (uint8_t index = 0U; index < count; ++index) {
        touch->data.coords[index] = points[index];
    }
    portEXIT_CRITICAL(&touch->data.lock);
    return ESP_OK;
}

}  // namespace micropixel::platform::input
