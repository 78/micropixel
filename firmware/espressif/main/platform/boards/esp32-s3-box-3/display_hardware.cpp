#include "platform/boards/esp32-s3-box-3/display_hardware.hpp"

#include <cstddef>
#include <cstdint>

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_tt21100.h"
#include "esp_log.h"
#include "platform/boards/esp32-s3-box-3/board_hardware.hpp"
#include "platform/boards/esp32-s3-box-3/platform_state.hpp"

namespace micropixel::platform::esp32_s3_box_3::detail {
namespace {

constexpr int kLcdCommandBits = 8;
constexpr int kLcdParameterBits = 8;
constexpr size_t kPanelIoQueueDepth = 10U;

constexpr uint8_t kPowerControlA[]{0xFF, 0x93, 0x42};
constexpr uint8_t kPowerControl1[]{0x0E, 0x0E};
constexpr uint8_t kVcomControl1[]{0xD0};
constexpr uint8_t kPowerControl2[]{0x02};
constexpr uint8_t kInversionControl[]{0x02};
constexpr uint8_t kPositiveGamma[]{0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39,
                                   0x48, 0x02, 0x0A, 0x08, 0x17, 0x17, 0x0F};
constexpr uint8_t kNegativeGamma[]{0x00, 0x28, 0x29, 0x01, 0x0D, 0x03, 0x3F, 0x33,
                                   0x52, 0x04, 0x0F, 0x0E, 0x37, 0x38, 0x0F};
constexpr uint8_t kFrameRate[]{0x00, 0x1B};
constexpr uint8_t kMemoryAccessControl[]{0x08};
constexpr uint8_t kPixelFormat[]{0x55};
constexpr uint8_t kEntryMode[]{0x06};

const ili9341_lcd_init_cmd_t kIli9341Initialization[]{
    {0xC8, kPowerControlA, sizeof(kPowerControlA), 0U},
    {0xC0, kPowerControl1, sizeof(kPowerControl1), 0U},
    {0xC5, kVcomControl1, sizeof(kVcomControl1), 0U},
    {0xC1, kPowerControl2, sizeof(kPowerControl2), 0U},
    {0xB4, kInversionControl, sizeof(kInversionControl), 0U},
    {0xE0, kPositiveGamma, sizeof(kPositiveGamma), 0U},
    {0xE1, kNegativeGamma, sizeof(kNegativeGamma), 0U},
    {0xB1, kFrameRate, sizeof(kFrameRate), 0U},
    {0x36, kMemoryAccessControl, sizeof(kMemoryAccessControl), 0U},
    {0x3A, kPixelFormat, sizeof(kPixelFormat), 0U},
    {0xB7, kEntryMode, sizeof(kEntryMode), 0U},
    {0x11, nullptr, 0U, 120U},
    {0x29, nullptr, 0U, 120U},
};

void ReleaseDisplayHardware(Box3BoardState& state, bool bus_initialized) {
    if (state.panel != nullptr) {
        (void)esp_lcd_panel_del(state.panel);
        state.panel = nullptr;
    }
    if (state.panel_io != nullptr) {
        (void)esp_lcd_panel_io_del(state.panel_io);
        state.panel_io = nullptr;
    }
    if (bus_initialized) {
        (void)spi_bus_free(kLcdSpiHost);
    }
}

}  // namespace

esp_err_t InitializeDisplayHardware(BoardHardware& hardware, Box3BoardState& state) {
    const int transfer_bytes = kWidth * CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT * 2;
    spi_bus_config_t bus_config{};
    bus_config.mosi_io_num = kLcdMosi;
    bus_config.miso_io_num = GPIO_NUM_NC;
    bus_config.sclk_io_num = kLcdClock;
    bus_config.quadwp_io_num = GPIO_NUM_NC;
    bus_config.quadhd_io_num = GPIO_NUM_NC;
    bus_config.data4_io_num = GPIO_NUM_NC;
    bus_config.data5_io_num = GPIO_NUM_NC;
    bus_config.data6_io_num = GPIO_NUM_NC;
    bus_config.data7_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = transfer_bytes;
    esp_err_t status = spi_bus_initialize(kLcdSpiHost, &bus_config, SPI_DMA_CH_AUTO);
    if (status != ESP_OK) {
        return status;
    }

    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = kLcdChipSelect;
    io_config.dc_gpio_num = kLcdDataCommand;
    io_config.spi_mode = 0;
    io_config.pclk_hz = kLcdPixelClockHz;
    io_config.trans_queue_depth = kPanelIoQueueDepth;
    io_config.lcd_cmd_bits = kLcdCommandBits;
    io_config.lcd_param_bits = kLcdParameterBits;
    io_config.flags.psram_dma_direct = 1;
    status = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kLcdSpiHost), &io_config, &state.panel_io);
    if (status != ESP_OK) {
        ReleaseDisplayHardware(state, true);
        return status;
    }

    const bool tt21100 = i2c_master_probe(hardware.I2cBus(), ESP_LCD_TOUCH_IO_I2C_TT21100_ADDRESS, 100) == ESP_OK;
    state.touch_name = tt21100 ? "TT21100" : "GT911";
    state.panel_name = tt21100 ? "ST7789" : "ILI9341";
    ili9341_vendor_config_t ili9341_config{
        .init_cmds = kIli9341Initialization,
        .init_cmds_size = static_cast<uint16_t>(sizeof(kIli9341Initialization) / sizeof(kIli9341Initialization[0])),
    };
    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.rgb_ele_order = kLcdColorOrder;
    panel_config.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    panel_config.bits_per_pixel = 16;
    panel_config.reset_gpio_num = kLcdReset;
    panel_config.vendor_config = tt21100 ? nullptr : &ili9341_config;
    panel_config.flags.reset_active_high = 1;
    status = tt21100 ? esp_lcd_new_panel_st7789(state.panel_io, &panel_config, &state.panel)
                     : esp_lcd_new_panel_ili9341(state.panel_io, &panel_config, &state.panel);
    if (status == ESP_OK) {
        status = esp_lcd_panel_reset(state.panel);
    }
    if (status == ESP_OK) {
        status = esp_lcd_panel_init(state.panel);
    }
    if (status == ESP_OK) {
        status = esp_lcd_panel_mirror(state.panel, true, true);
    }
    if (status != ESP_OK) {
        ReleaseDisplayHardware(state, true);
        return status;
    }

    ESP_LOGI(kTag, "panel IO uses direct PSRAM SPI DMA: block=%d bytes clock=%d MHz", transfer_bytes,
             static_cast<int>(kLcdPixelClockHz / 1000000U));
    return ESP_OK;
}

esp_err_t InitializeTouchHardware(BoardHardware& hardware, Box3BoardState& state) {
    esp_lcd_touch_config_t touch_config{};
    touch_config.x_max = kWidth;
    touch_config.y_max = kHeight;
    touch_config.rst_gpio_num = GPIO_NUM_NC;
    touch_config.int_gpio_num = kTouchInterrupt;
    touch_config.levels.reset = 0;
    touch_config.levels.interrupt = 0;

    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.scl_speed_hz = 100000U;
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 0;
    io_config.lcd_cmd_bits = 16;
    io_config.flags.disable_control_phase = 1;

    enum class Controller { kGt911, kTt21100 } controller{};
    if (i2c_master_probe(hardware.I2cBus(), ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 100) == ESP_OK) {
        io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
        controller = Controller::kGt911;
    } else if (i2c_master_probe(hardware.I2cBus(), ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 100) == ESP_OK) {
        io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        controller = Controller::kGt911;
    } else if (i2c_master_probe(hardware.I2cBus(), ESP_LCD_TOUCH_IO_I2C_TT21100_ADDRESS, 100) == ESP_OK) {
        io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_TT21100_ADDRESS;
        touch_config.flags.mirror_x = true;
        controller = Controller::kTt21100;
    } else {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(hardware.I2cBus(), &io_config, &state.touch_io), kTag,
                        "create touch panel IO failed");
    if (controller == Controller::kTt21100) {
        return esp_lcd_touch_new_i2c_tt21100(state.touch_io, &touch_config, &state.touch);
    }
    return esp_lcd_touch_new_i2c_gt911(state.touch_io, &touch_config, &state.touch);
}

}  // namespace micropixel::platform::esp32_s3_box_3::detail
