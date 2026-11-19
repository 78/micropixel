// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/reterminal-d1001/power_key.hpp"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "platform/boards/reterminal-d1001/board_config.hpp"
#include "platform/boards/reterminal-d1001/power_controller.hpp"

namespace micropixel::platform::reterminal_d1001 {
namespace {

constexpr char kTag[] = "d1001_key";
constexpr uint64_t kPollIntervalUs = 20'000U;
constexpr uint64_t kLongPressUs = static_cast<uint64_t>(board::kPowerOffLongPressMs) * 1000U;

}  // namespace

D1001PowerKey::~D1001PowerKey() {
    if (timer_ != nullptr) {
        (void)esp_timer_stop_blocking(timer_, portMAX_DELAY);
        (void)esp_timer_delete(timer_);
    }
}

esp_err_t D1001PowerKey::Initialize() {
    if (timer_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << board::kPowerKey;
    config.mode = GPIO_MODE_INPUT;
    // The D1001 button circuit drives GPIO3 high when pressed; the BSP
    // disables the internal pulls and relies on the external network.
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&config), kTag, "configure power key failed");

    esp_timer_create_args_t timer_args{};
    timer_args.callback = Timer;
    timer_args.arg = this;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "d1001_key";
    timer_args.skip_unhandled_events = true;
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &timer_), kTag, "create power key timer failed");
    return esp_timer_start_periodic(timer_, kPollIntervalUs);
}

void D1001PowerKey::Timer(void* context) { static_cast<D1001PowerKey*>(context)->Poll(); }

void D1001PowerKey::Poll() {
    const bool down = gpio_get_level(board::kPowerKey) == 1;
    const uint64_t now = esp_timer_get_time();
    if (down && !pressed_) {
        pressed_ = true;
        long_fired_ = false;
        pressed_at_us_ = now;
        return;
    }
    if (!down && pressed_) {
        pressed_ = false;
        if (!long_fired_ && now - pressed_at_us_ < kLongPressUs) {
            (void)power_.NotifyClick(now);
        }
        return;
    }
    if (down && pressed_ && !long_fired_ && now - pressed_at_us_ >= kLongPressUs) {
        long_fired_ = true;
        (void)power_.NotifyLongPress(now);
    }
}

}  // namespace micropixel::platform::reterminal_d1001