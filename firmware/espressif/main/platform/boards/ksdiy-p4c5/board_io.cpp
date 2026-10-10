#include "platform/boards/ksdiy-p4c5/board_io.hpp"

#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "ksdiy_lvgl_port.h"
#include "platform/boards/ksdiy-p4c5/board_config.hpp"
#include "platform/controllers/brightness_curve.hpp"

#if !CONFIG_KSDIY_P4C5_LCD_RGB888
#error "KSDIY P4C5 MicroPixel builds scan out RGB888; select CONFIG_KSDIY_P4C5_LCD_RGB888"
#endif

namespace micropixel::platform::ksdiy_p4c5 {
namespace {

constexpr char kTag[] = "ksdiy_p4c5_io";
constexpr uint32_t kBspBrightnessMax = 255U;

}  // namespace

esp_err_t BoardIo::Initialize() {
    if (board::kLandscape) {
        ESP_RETURN_ON_ERROR(ksdiy_lvgl_port_set_rotation(90), kTag, "select BSP 90-degree rotation failed");
    }
    // Brings up the backlight PWM, LCD/touch reset, DSI PHY LDO, I2C0, the
    // AXP2101 codec rails, the ST7102 panel and the ST7123 touch controller.
    // The BSP aborts on a hardware initialization failure.
    ksdiy_panel_bare_init();
    // The bare-panel path lights the backlight at once; keep it dark until the
    // first LVGL frame is on the panel.
    ESP_RETURN_ON_ERROR(ksdiy_lcd_set_brightness(0), kTag, "turn backlight off failed");

    i2c_bus_ = touch_i2c_bus_;
    panel_ = ksdiy_lvgl_get_panel_handle();
    touch_ = ksdiy_touch_get_handle();
    ESP_RETURN_ON_FALSE(i2c_bus_ != nullptr && panel_ != nullptr && touch_ != nullptr, ESP_ERR_INVALID_STATE, kTag,
                        "BSP did not provide the I2C bus, panel or touch handle");
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_enable_dma2d(panel_), kTag, "enable DPI DMA2D failed");
    AttachPmic();
    return ESP_OK;
}

void BoardIo::AttachPmic() {
    // The BSP already configured the codec rails. A PMIC that does not answer
    // only costs the battery indicator.
    esp_err_t status = i2c_master_probe(i2c_bus_, board::kPmicI2cAddress, 50);
    if (status == ESP_OK) {
        status = pmic_.Attach(i2c_bus_, board::kPmicI2cAddress);
    }
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "AXP2101 battery state unavailable: %s", esp_err_to_name(status));
        return;
    }
    pmic_available_ = true;
}

esp_err_t BoardIo::SetDisplayEnabled(bool enabled) {
    if (panel_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!enabled) {
        ESP_RETURN_ON_ERROR(SetBacklightOutputPerTenThousand(0U), kTag, "turn backlight off failed");
    }
    return esp_lcd_panel_disp_on_off(panel_, enabled);
}

esp_err_t BoardIo::SetBacklightOutputPerTenThousand(uint32_t output) {
    const uint32_t clamped_output =
        output <= controllers::kBrightnessControlScale ? output : controllers::kBrightnessControlScale;
    uint32_t level = (kBspBrightnessMax * clamped_output + controllers::kBrightnessControlScale / 2U) /
                     controllers::kBrightnessControlScale;
    if (clamped_output != 0U && level == 0U) {
        level = 1U;
    }
    return ksdiy_lcd_set_brightness(static_cast<uint8_t>(level));
}

}  // namespace micropixel::platform::ksdiy_p4c5
