#include "platform/boards/esp-mosaico/power_controller.hpp"

#include <cinttypes>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/boards/esp-mosaico/board_config.hpp"

namespace micropixel::platform::esp_mosaico {
namespace {

constexpr char kTag[] = "mosaico_power";
constexpr uint32_t kCodecPowerStabilizationMs = 10U;

esp_err_t ConfigureOutput(gpio_num_t pin, int level, gpio_mode_t mode = GPIO_MODE_OUTPUT) {
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, level), kTag, "preset GPIO%d failed", static_cast<int>(pin));
    gpio_config_t config{};
    config.pin_bit_mask = BIT64(pin);
    config.mode = mode;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}

}  // namespace

esp_err_t PowerController::Initialize() {
    if (initialized_) {
        return ESP_OK;
    }
    // Deep-sleep pad holds survive reset. Program safe inactive levels into the
    // output latches before releasing a retained state, matching the board BSP.
    ESP_RETURN_ON_ERROR(ConfigureOutput(board::kPeripheralPower, 1), kTag, "configure peripheral power control failed");
    ESP_RETURN_ON_ERROR(ConfigureOutput(board::kAudioCodecPower, 0), kTag, "configure codec power control failed");
    ESP_RETURN_ON_ERROR(ConfigureOutput(board::kPowerSwitch, 1, GPIO_MODE_OUTPUT_OD), kTag,
                        "release board power switch failed");
    ESP_RETURN_ON_ERROR(gpio_hold_dis(board::kPeripheralPower), kTag, "release peripheral power GPIO hold failed");
    ESP_RETURN_ON_ERROR(gpio_hold_dis(board::kAudioCodecPower), kTag, "release codec power GPIO hold failed");
    ESP_RETURN_ON_ERROR(gpio_hold_dis(board::kPowerSwitch), kTag, "release board power switch GPIO hold failed");

    initialized_ = true;
    peripheral_power_enabled_ = false;
    codec_power_enabled_ = false;
    const esp_err_t status = SetPeripheralPower(true);
    if (status != ESP_OK) {
        initialized_ = false;
    }
    return status;
}

esp_err_t PowerController::SetPeripheralPower(bool enabled) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!enabled || peripheral_power_enabled_) {
        const int level = enabled ? 0 : 1;
        ESP_RETURN_ON_ERROR(gpio_set_level(board::kPeripheralPower, level), kTag, "set peripheral power failed");
        peripheral_power_enabled_ = enabled;
        return ESP_OK;
    }

    constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
    constexpr ledc_timer_t kTimer = LEDC_TIMER_1;
    constexpr ledc_channel_t kChannel = LEDC_CHANNEL_1;
    constexpr ledc_timer_bit_t kResolution = LEDC_TIMER_8_BIT;
    constexpr uint32_t kMaximumDuty = (1U << kResolution) - 1U;

    ledc_timer_config_t timer{};
    timer.speed_mode = kSpeedMode;
    timer.duty_resolution = kResolution;
    timer.timer_num = kTimer;
    timer.freq_hz = 100000U;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), kTag, "configure peripheral power ramp timer failed");

    ledc_channel_config_t channel{};
    channel.gpio_num = board::kPeripheralPower;
    channel.speed_mode = kSpeedMode;
    channel.channel = kChannel;
    channel.timer_sel = kTimer;
    channel.duty = kMaximumDuty;
    channel.hpoint = 0U;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), kTag, "configure peripheral power ramp channel failed");

    const esp_err_t installed = ledc_fade_func_install(0U);
    esp_err_t status = installed == ESP_ERR_INVALID_STATE ? ESP_OK : installed;
    if (status == ESP_OK) {
        status = ledc_set_fade_with_time(kSpeedMode, kChannel, 0U, board::kPowerRampMs);
    }
    if (status == ESP_OK) {
        status = ledc_fade_start(kSpeedMode, kChannel, LEDC_FADE_WAIT_DONE);
    }
    if (installed == ESP_OK) {
        ledc_fade_func_uninstall();
    }
    ledc_stop(kSpeedMode, kChannel, status == ESP_OK ? 0 : 1);
    ESP_RETURN_ON_ERROR(status, kTag, "ramp peripheral power failed");
    ESP_RETURN_ON_ERROR(ConfigureOutput(board::kPeripheralPower, 0), kTag, "hold peripheral power on failed");
    peripheral_power_enabled_ = true;
    ESP_LOGI(kTag, "3V3 peripheral rail ramped on over %" PRIu32 " ms", board::kPowerRampMs);
    return ESP_OK;
}

esp_err_t PowerController::SetCodecPower(bool enabled) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(gpio_set_level(board::kAudioCodecPower, enabled ? 1 : 0), kTag, "set codec power failed");
    codec_power_enabled_ = enabled;
    ESP_LOGI(kTag, "codec 3V3 rail %s", enabled ? "on" : "off");
    return ESP_OK;
}

esp_err_t PowerController::PowerOnAudio() {
    ESP_RETURN_ON_ERROR(SetPeripheralPower(true), kTag, "ensure shared 3V3 rail is on failed");
    if (codec_power_enabled_) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(SetCodecPower(true), kTag, "enable codec 3V3 rail failed");
    // A previous boot-time initialization often hid this interval behind I2S
    // setup. On-demand wake accesses ES8311 immediately, so make the board BSP's
    // 10 ms stabilization requirement explicit here.
    vTaskDelay(pdMS_TO_TICKS(kCodecPowerStabilizationMs));
    return ESP_OK;
}

esp_err_t PowerController::PowerOffAudio() { return codec_power_enabled_ ? SetCodecPower(false) : ESP_OK; }

void PowerController::SetPowerButtonSink(device::PowerButtonSink, void*) {}

void PowerController::SetPowerOffButtonSink(device::PowerOffButtonSink, void*) {}

std::expected<void, device::PowerError> PowerController::EnterLowPower() {
    return std::unexpected(device::PowerError::kUnavailable);
}

[[noreturn]] void PowerController::PowerOff() {
    (void)ConfigureOutput(board::kPowerSwitch, 0, GPIO_MODE_OUTPUT_OD);
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

}  // namespace micropixel::platform::esp_mosaico
