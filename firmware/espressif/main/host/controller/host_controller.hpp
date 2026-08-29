#ifndef MICROPIXEL_FIRMWARE_HOST_CONTROLLER_HPP
#define MICROPIXEL_FIRMWARE_HOST_CONTROLLER_HPP

#include "host/controller/host_power_state.hpp"

namespace micropixel::device {
class Battery;
class DeviceServices;
class Power;
class Wifi;
}  // namespace micropixel::device

namespace micropixel::host_ui {
class SystemShell;
}

namespace micropixel::work {
class BackgroundExecutor;
}

namespace micropixel::firmware {
namespace control {
class ControlDispatcher;
class GuestLogBuffer;
}  // namespace control
namespace remote_control {
class RemoteControlAgent;
}

// Owns Host-level App selection and lifecycle policy. FirmwareApp remains the
// composition root; this controller performs explicit Hall/Foreground state
// transitions after the platform has initialized.
class HostController final {
   public:
    HostController(device::DeviceServices& devices, device::Battery& battery, device::Wifi& wifi, device::Power& power,
                   host_ui::SystemShell& shell, control::ControlDispatcher& controls,
                   control::GuestLogBuffer& guest_logs, remote_control::RemoteControlAgent& remote_control,
                   work::BackgroundExecutor& background_executor);
    ~HostController();
    HostController(const HostController&) = delete;
    HostController& operator=(const HostController&) = delete;

    void Run();

   private:
    device::DeviceServices& devices_;
    device::Battery& battery_;
    device::Wifi& wifi_;
    device::Power& power_;
    host_ui::SystemShell& shell_;
    control::ControlDispatcher& controls_;
    control::GuestLogBuffer& guest_logs_;
    remote_control::RemoteControlAgent& remote_control_;
    work::BackgroundExecutor& background_executor_;
    HostPowerStateMachine power_state_{};
};

}  // namespace micropixel::firmware

#endif
