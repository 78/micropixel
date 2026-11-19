// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/reterminal-d1001/power_controller.hpp"

#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/boards/reterminal-d1001/board_hardware.hpp"

namespace micropixel::platform::reterminal_d1001 {
namespace {

constexpr char kTag[] = "d1001_power";
constexpr uint32_t kBacklightSettleMs = 120U;

}  // namespace

std::expected<void, device::PowerError> D1001PowerController::EnterLowPower() {
    // The Host has already faded the backlight out. Latch the screen-off state
    // so the next key click is consumed as a wake request instead of being
    // forwarded to the Host, then wait for that request.
    screen_off_.store(true, std::memory_order_release);
    wake_requested_.store(false, std::memory_order_release);
    (void)hardware_.SetBrightness(0);
    while (!wake_requested_.load(std::memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    screen_off_.store(false, std::memory_order_release);
    return {};
}

[[noreturn]] void D1001PowerController::PowerOff() {
    ESP_LOGI(kTag, "power off: backlight off, releasing the 3V3 rail latch");
    (void)hardware_.SetBrightness(0);
    vTaskDelay(pdMS_TO_TICKS(kBacklightSettleMs));
    // P1.0 is the PMIC power-hold; clearing it turns the board off when running
    // from the battery. On USB power the SoC still stops through deep sleep.
    (void)hardware_.SetPowerHold(false);
    vTaskDelay(pdMS_TO_TICKS(kBacklightSettleMs));
    esp_deep_sleep_start();
}

}  // namespace micropixel::platform::reterminal_d1001