#include "platform/boards/szpi-esp32s3/board_hardware.hpp"

#include <algorithm>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "platform/buses/i2c_executor.hpp"

namespace micropixel::platform::szpi_esp32s3 {
namespace {

constexpr char kTag[] = "szpi_s3_hw";
constexpr i2c_port_num_t kI2cPort = I2C_NUM_0;
constexpr gpio_num_t kI2cSda = GPIO_NUM_1;
constexpr gpio_num_t kI2cScl = GPIO_NUM_2;
constexpr uint16_t kPca9557Address = 0x19U;
constexpr uint8_t kPca9557OutputPort = 0x01U;
constexpr uint8_t kPca9557ConfigurationPort = 0x03U;
constexpr uint8_t kLcdChipSelect = 1U << 0U;
constexpr uint8_t kAmplifierEnable = 1U << 1U;
constexpr gpio_num_t kBacklight = GPIO_NUM_42;
constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;
constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_1;
constexpr uint32_t kBacklightMaximumDuty = (1U << 10U) - 1U;

esp_err_t WriteRegister(i2c_master_dev_handle_t device, uint8_t address, uint8_t value) {
    const uint8_t bytes[]{address, value};
    return i2c_master_transmit(device, bytes, sizeof(bytes), 100);
}

}  // namespace

esp_err_t BoardHardware::Initialize() {
    if (i2c_bus_ != nullptr || expander_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = kI2cPort;
    bus_config.sda_io_num = kI2cSda;
    bus_config.scl_io_num = kI2cScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7U;
    bus_config.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &i2c_bus_), kTag, "create shared I2C bus failed");

    i2c_device_config_t expander_config{};
    expander_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    expander_config.device_address = kPca9557Address;
    expander_config.scl_speed_hz = 100000U;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus_, &expander_config, &expander_), kTag,
                        "attach PCA9557 failed");

    // Match the reference board sequence: keep LCD CS high until the panel
    // handle has been created and reset has completed. The unused upper five
    // expander pins stay inputs.
    expander_output_ = static_cast<uint8_t>(kLcdChipSelect | kAmplifierEnable);
    ESP_RETURN_ON_ERROR(WriteExpanderOutput(expander_output_), kTag, "preset PCA9557 outputs failed");
    ESP_RETURN_ON_ERROR(WriteRegister(expander_, kPca9557ConfigurationPort, 0xf8U), kTag,
                        "configure PCA9557 directions failed");

    ledc_timer_config_t timer_config{};
    timer_config.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_config.duty_resolution = LEDC_TIMER_10_BIT;
    timer_config.timer_num = kBacklightTimer;
    timer_config.freq_hz = 5000U;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), kTag, "configure backlight timer failed");
    ledc_channel_config_t channel_config{};
    channel_config.gpio_num = kBacklight;
    channel_config.speed_mode = LEDC_LOW_SPEED_MODE;
    channel_config.channel = kBacklightChannel;
    channel_config.timer_sel = kBacklightTimer;
    channel_config.duty = 0U;
    channel_config.hpoint = 0;
    channel_config.flags.output_invert = true;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), kTag, "configure backlight channel failed");
    brightness_initialized_ = true;
    return ESP_OK;
}

esp_err_t BoardHardware::SelectDisplay() {
    const uint8_t next = static_cast<uint8_t>(expander_output_ & ~kLcdChipSelect);
    ESP_RETURN_ON_ERROR(WriteExpanderOutput(next), kTag, "select LCD failed");
    expander_output_ = next;
    return ESP_OK;
}

esp_err_t BoardHardware::SetBrightness(int percent) {
    if (!brightness_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t bounded = static_cast<uint32_t>(std::clamp(percent, 0, 100));
    const uint32_t duty = (kBacklightMaximumDuty * bounded + 50U) / 100U;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, kBacklightChannel, duty), kTag, "set backlight duty failed");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, kBacklightChannel);
}

esp_err_t BoardHardware::SetAmplifier(bool enabled, buses::I2cExecutor& executor) {
    struct Request final {
        BoardHardware* hardware;
        bool enabled;
    } request{this, enabled};
    return executor.Invoke(
        buses::I2cExecutor::Priority::kHigh,
        [](void* context) {
            auto& requested = *static_cast<Request*>(context);
            const uint8_t next = requested.enabled
                                     ? static_cast<uint8_t>(requested.hardware->expander_output_ | kAmplifierEnable)
                                     : static_cast<uint8_t>(requested.hardware->expander_output_ & ~kAmplifierEnable);
            const esp_err_t status = requested.hardware->WriteExpanderOutput(next);
            if (status == ESP_OK) {
                requested.hardware->expander_output_ = next;
            }
            return status;
        },
        &request);
}

esp_err_t BoardHardware::WriteExpanderOutput(uint8_t value) {
    if (expander_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return WriteRegister(expander_, kPca9557OutputPort, value);
}

}  // namespace micropixel::platform::szpi_esp32s3
