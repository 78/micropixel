// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/esp-mosaico/board_config.hpp"

#include <cassert>

#include "esp_check.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "platform/memory/ext_ram_bss.hpp"

namespace micropixel::platform::esp_mosaico::board {
namespace {

constexpr char kTag[] = "mosaico_board";
MICROPIXEL_EXT_RAM_BSS std::optional<HardwareConfig> hardware;

}  // namespace

esp_err_t DetectHardware() {
    if (hardware.has_value()) {
        return ESP_OK;
    }
    uint16_t version = 0U;
    ESP_RETURN_ON_ERROR(esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &version, sizeof(version) * 8U), kTag,
                        "read hardware version failed");
    hardware = DecodeHardwareConfig(version);
    if (!hardware.has_value()) {
        ESP_LOGE(kTag, "unsupported hardware version: 0x%04x", static_cast<unsigned>(version));
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(kTag, "%s: LCD reset=%d clock=%d, onboard I2C SDA=%d SCL=%d, codec power=%d LED=%d", hardware->name,
             hardware->display_reset, hardware->display_clock, hardware->i2c_data, hardware->i2c_clock,
             hardware->codec_power, hardware->status_led);
    return ESP_OK;
}

const HardwareConfig& Hardware() {
    assert(hardware.has_value());
    return *hardware;
}

}  // namespace micropixel::platform::esp_mosaico::board
