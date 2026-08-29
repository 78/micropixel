#include "host/controller/host_power_coordinator.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/controller/host_power_state.hpp"
#include "host/controller/remote/remote_control_agent.hpp"
#include "host/ui/system_shell.hpp"

namespace micropixel::firmware::host_power {
namespace {

constexpr char kTag[] = "micropixel_power";
constexpr uint32_t kBrightnessFadeSteps = 12U;
constexpr TickType_t kBrightnessFadeStepDelay = pdMS_TO_TICKS(15U);
constexpr TickType_t kShutdownRemoteStopTimeout = pdMS_TO_TICKS(500U);

}  // namespace

const char* ErrorText(device::PowerError error) {
    switch (error) {
        case device::PowerError::kUnavailable:
            return "unavailable";
        case device::PowerError::kWakeSource:
            return "wake_source";
        case device::PowerError::kDisplayPrepare:
            return "display_prepare";
        case device::PowerError::kDisplayShutdown:
            return "display_shutdown";
        case device::PowerError::kSleepRejected:
            return "sleep_rejected";
        case device::PowerError::kDisplayRestore:
        default:
            return "display_restore";
    }
}

void FadeBrightness(host_ui::SystemShell& shell, uint8_t from, uint8_t to) {
    for (uint32_t step = 1U; step <= kBrightnessFadeSteps; ++step) {
        const int32_t value = static_cast<int32_t>(from) + (static_cast<int32_t>(to) - static_cast<int32_t>(from)) *
                                                               static_cast<int32_t>(step) /
                                                               static_cast<int32_t>(kBrightnessFadeSteps);
        shell.ApplyBrightness(static_cast<uint8_t>(value));
        if (step != kBrightnessFadeSteps) {
            vTaskDelay(kBrightnessFadeStepDelay);
        }
    }
}

void RunBasicShutdown(host_ui::SystemShell& shell, device::Power& power, HostPowerStateMachine& power_state,
                      remote_control::RemoteControlAgent& remote_control) {
    if (!shell.PowerOffRequested() || !power_state.BeginShutdown()) {
        return;
    }
    if (!shell.ConsumePowerOffRequested()) {
        power_state.RecoverAwake();
        return;
    }

    ESP_LOGI(kTag, "power-off sequence started without a running App");
    const auto show_result = shell.ShowShutdown();
    if (!show_result) {
        ESP_LOGE(kTag, "could not show shutdown screen: error=%u", static_cast<unsigned>(show_result.error()));
    }
    shell.ApplyVolume(0U);
    remote_control.Stop(kShutdownRemoteStopTimeout);
    power.PowerOff();
}

void RunBasicPowerCycle(host_ui::SystemShell& shell, const host_ui::StatusLayerModel& status_model,
                        device::Power& power, HostPowerStateMachine& power_state) {
    if (!shell.PowerButtonPressed() || !power_state.BeginSleep()) {
        return;
    }
    if (!shell.ConsumePowerButtonPressed()) {
        power_state.RecoverAwake();
        return;
    }
    FadeBrightness(shell, status_model.brightness_percent, 0U);
    if (!power_state.MarkAsleep()) {
        power_state.RecoverAwake();
        FadeBrightness(shell, 0U, status_model.brightness_percent);
        shell.NotifyPowerCycleCompleted();
        return;
    }
    const auto sleep_result = power.EnterLowPower();
    if (!power_state.BeginWake()) {
        power_state.RecoverAwake();
    }
    if (!sleep_result) {
        ESP_LOGE(kTag, "low-power cycle failed: %s", ErrorText(sleep_result.error()));
    }
    FadeBrightness(shell, 0U, status_model.brightness_percent);
    if (!power_state.FinishWake()) {
        power_state.RecoverAwake();
    }
    shell.NotifyPowerCycleCompleted();
}

}  // namespace micropixel::firmware::host_power
