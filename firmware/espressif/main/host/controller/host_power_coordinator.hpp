#pragma once

#include <cstdint>

#include "device/contracts/power.hpp"

namespace micropixel::device {
class Power;
}

namespace micropixel::host_ui {
class SystemShell;
struct StatusLayerModel;
}  // namespace micropixel::host_ui

namespace micropixel::firmware {
class HostPowerStateMachine;
namespace remote_control {
class RemoteControlAgent;
}

namespace host_power {

[[nodiscard]] const char* ErrorText(device::PowerError error);
void FadeBrightness(host_ui::SystemShell& shell, uint8_t from, uint8_t to);
void RunBasicShutdown(host_ui::SystemShell& shell, device::Power& power, HostPowerStateMachine& power_state,
                      remote_control::RemoteControlAgent& remote_control);
void RunBasicPowerCycle(host_ui::SystemShell& shell, const host_ui::StatusLayerModel& status_model,
                        device::Power& power, HostPowerStateMachine& power_state);

}  // namespace host_power
}  // namespace micropixel::firmware
