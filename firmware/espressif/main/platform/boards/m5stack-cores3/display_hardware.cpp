#include "platform/boards/m5stack-cores3/display_hardware.hpp"

#include <cstddef>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "platform/boards/esp32-s3-common/landscape_320_state.hpp"
#include "platform/boards/m5stack-cores3/board_hardware.hpp"

namespace micropixel::platform::m5stack_cores3 {
namespace {

constexpr char kTag[] = "cores3_display";
constexpr spi_host_device_t kLcdSpiHost = SPI3_HOST;
constexpr gpio_num_t kLcdMosi = GPIO_NUM_37;
constexpr gpio_num_t kLcdClock = GPIO_NUM_36;
constexpr gpio_num_t kLcdDataCommand = GPIO_NUM_35;
constexpr gpio_num_t kLcdChipSelect = GPIO_NUM_3;
constexpr uint32_t kLcdPixelClockHz = 40U * 1000U * 1000U;
constexpr size_t kPanelIoQueueDepth = 10U;

void ReleaseDisplayHardware(esp32_s3_common::Landscape320State& state, bool bus_initialized) {
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

esp_err_t InitializeDisplayHardware(BoardHardware& hardware, esp32_s3_common::Landscape320State& state) {
    ESP_RETURN_ON_ERROR(hardware.Initialize(), kTag, "initialize shared board hardware failed");
    ESP_RETURN_ON_ERROR(hardware.EnableDisplayAndTouch(), kTag, "enable display and touch rails failed");
    const int transfer_bytes = esp32_s3_common::kWidth * CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT * 2;
    spi_bus_config_t bus_config{};
    bus_config.mosi_io_num = kLcdMosi;
    // GPIO35 is shared with SD MISO on CoreS3, but acts only as LCD D/C
    // until an SD device is attached to this bus.
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
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.flags.psram_dma_direct = 1;
    status = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kLcdSpiHost), &io_config, &state.panel_io);
    if (status != ESP_OK) {
        ReleaseDisplayHardware(state, true);
        return status;
    }

    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_config.bits_per_pixel = 16;
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    status = esp_lcd_new_panel_ili9341(state.panel_io, &panel_config, &state.panel);
    if (status == ESP_OK) {
        status = esp_lcd_panel_reset(state.panel);
    }
    if (status == ESP_OK) {
        status = esp_lcd_panel_init(state.panel);
    }
    if (status == ESP_OK) {
        status = esp_lcd_panel_invert_color(state.panel, true);
    }
    if (status == ESP_OK) {
        status = esp_lcd_panel_swap_xy(state.panel, false);
    }
    if (status == ESP_OK) {
        status = esp_lcd_panel_mirror(state.panel, false, false);
    }
    if (status == ESP_OK) {
        status = esp_lcd_panel_disp_on_off(state.panel, true);
    }
    if (status != ESP_OK) {
        ReleaseDisplayHardware(state, true);
        return status;
    }
    state.panel_name = "ILI9342C";
    ESP_LOGI(kTag, "ILI9342C panel uses direct PSRAM SPI DMA: block=%d bytes clock=%lu MHz", transfer_bytes,
             static_cast<unsigned long>(kLcdPixelClockHz / 1000000U));
    return ESP_OK;
}

esp_err_t InitializeTouch(BoardHardware& hardware, esp32_s3_common::Landscape320State& state) {
    esp_lcd_touch_config_t touch_config{};
    touch_config.x_max = esp32_s3_common::kWidth;
    touch_config.y_max = esp32_s3_common::kHeight;
    touch_config.rst_gpio_num = GPIO_NUM_NC;
    touch_config.int_gpio_num = GPIO_NUM_NC;
    touch_config.levels.reset = 0;
    touch_config.levels.interrupt = 0;
    touch_config.flags.swap_xy = false;
    touch_config.flags.mirror_x = false;
    touch_config.flags.mirror_y = false;
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS;
    io_config.scl_speed_hz = 100000U;
    io_config.control_phase_bytes = 1U;
    io_config.dc_bit_offset = 0U;
    io_config.lcd_cmd_bits = 8U;
    io_config.flags.disable_control_phase = 1U;
    const auto initialize = [&]() {
        esp_err_t status = esp_lcd_new_panel_io_i2c(hardware.I2cBus(), &io_config, &state.touch_io);
        if (status == ESP_OK) {
            status = esp_lcd_touch_new_i2c_ft5x06(state.touch_io, &touch_config, &state.touch);
        }
        return status;
    };
    const auto release = [&]() {
        if (state.touch != nullptr) {
            (void)esp_lcd_touch_del(state.touch);
            state.touch = nullptr;
        }
        if (state.touch_io != nullptr) {
            (void)esp_lcd_panel_io_del(state.touch_io);
            state.touch_io = nullptr;
        }
    };
    esp_err_t status = initialize();
    if (status != ESP_OK) {
        release();
        ESP_RETURN_ON_ERROR(hardware.RecoverTouch(), kTag, "recover FT6336U power failed");
        status = initialize();
    }
    if (status != ESP_OK) {
        release();
        return status;
    }
    state.touch_name = "FT6336U";
    return ESP_OK;
}

}  // namespace micropixel::platform::m5stack_cores3
