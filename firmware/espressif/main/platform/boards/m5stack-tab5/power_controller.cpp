// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/m5stack-tab5/power_controller.hpp"

#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/boards/m5stack-tab5/board_hardware.hpp"

namespace micropixel::platform::m5stack_tab5 {
namespace {

constexpr char kTag[] = "tab5_power";
// Give the backlight a moment to settle before the SoC stops driving the panel.
constexpr uint32_t kBacklightSettleMs = 120U;

}  // namespace

[[noreturn]] void Tab5PowerController::PowerOff() {
    // The Host has already run its shutdown sequence (shutdown screen, volume,
    // remote stop), so the board's remaining duty is to stop the panel light
    // and cut the SoC: deep sleep keeps the expander and charger state as they
    // are, and the reset button restarts the board through a normal cold boot.
    // The expander's power-latch pulse is deliberately not used here: with no
    // power key on the enclosure it would leave the board off until USB power
    // is re-applied.
    ESP_LOGI(kTag, "power off: backlight off, entering deep sleep (reset reboots)");
    (void)hardware_.SetBrightness(0);
    vTaskDelay(pdMS_TO_TICKS(kBacklightSettleMs));
    esp_deep_sleep_start();
}

}  // namespace micropixel::platform::m5stack_tab5
