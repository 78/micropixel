#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_types.h"

namespace micropixel::platform::esp32_s3_box_3 {

inline constexpr i2c_port_num_t kI2cPort = I2C_NUM_1;
inline constexpr gpio_num_t kI2cSda = GPIO_NUM_8;
inline constexpr gpio_num_t kI2cScl = GPIO_NUM_18;

inline constexpr spi_host_device_t kLcdSpiHost = SPI2_HOST;
inline constexpr gpio_num_t kLcdMosi = GPIO_NUM_6;
inline constexpr gpio_num_t kLcdClock = GPIO_NUM_7;
inline constexpr gpio_num_t kLcdChipSelect = GPIO_NUM_5;
inline constexpr gpio_num_t kLcdDataCommand = GPIO_NUM_4;
inline constexpr gpio_num_t kLcdReset = GPIO_NUM_48;
inline constexpr gpio_num_t kLcdBacklight = GPIO_NUM_47;
inline constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_3;
inline constexpr uint32_t kLcdPixelClockHz = 40'000'000U;
inline constexpr lcd_rgb_element_order_t kLcdColorOrder = LCD_RGB_ELEMENT_ORDER_BGR;

inline constexpr int kI2sPort = 1;
inline constexpr gpio_num_t kI2sMasterClock = GPIO_NUM_2;
inline constexpr gpio_num_t kI2sBitClock = GPIO_NUM_17;
inline constexpr gpio_num_t kI2sWordSelect = GPIO_NUM_45;
inline constexpr gpio_num_t kI2sDataOut = GPIO_NUM_15;
inline constexpr gpio_num_t kAmplifierEnable = GPIO_NUM_46;
inline constexpr gpio_num_t kMuteStatus = GPIO_NUM_1;

class BoardHardware final {
   public:
    [[nodiscard]] esp_err_t Initialize();
    [[nodiscard]] esp_err_t SetBrightness(int percent);
    [[nodiscard]] i2c_master_bus_handle_t I2cBus() const { return i2c_bus_; }

   private:
    i2c_master_bus_handle_t i2c_bus_{};
    bool brightness_initialized_{};
};

}  // namespace micropixel::platform::esp32_s3_box_3
