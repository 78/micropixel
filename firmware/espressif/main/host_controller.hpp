#ifndef MICROPIXEL_FIRMWARE_HOST_CONTROLLER_HPP
#define MICROPIXEL_FIRMWARE_HOST_CONTROLLER_HPP

#include "host_power_state.hpp"

namespace micropixel::device {
class BatteryBackend;
class DeviceServices;
class PowerBackend;
class WifiBackend;
}  // namespace micropixel::device

namespace micropixel::host_ui {
class SystemShell;
}

namespace micropixel::firmware {
namespace remote_control {
class RemoteControlAgent;
}

// Owns Host-level App selection and lifecycle policy. FirmwareApp remains the
// composition root; this controller performs explicit Hall/Foreground state
// transitions after the platform has initialized.
class HostController final {
   public:
    HostController(device::DeviceServices& devices, device::BatteryBackend& battery, device::WifiBackend& wifi,
                   device::PowerBackend& power, host_ui::SystemShell& shell,
                   remote_control::RemoteControlAgent& remote_control);
    ~HostController();
    HostController(const HostController&) = delete;
    HostController& operator=(const HostController&) = delete;

    void Run();

   private:
    device::DeviceServices& devices_;
    device::BatteryBackend& battery_;
    device::WifiBackend& wifi_;
    device::PowerBackend& power_;
    host_ui::SystemShell& shell_;
    remote_control::RemoteControlAgent& remote_control_;
    HostPowerStateMachine power_state_{};
};

}  // namespace micropixel::firmware

#endif
