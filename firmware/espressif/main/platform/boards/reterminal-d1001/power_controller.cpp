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