#ifndef MICROPIXEL_FIRMWARE_HOST_CONTROLLER_HPP
#define MICROPIXEL_FIRMWARE_HOST_CONTROLLER_HPP

namespace micropixel::device {
class DeviceServices;
class WifiBackend;
}  // namespace micropixel::device

namespace micropixel::host_ui {
class SystemShell;
}

namespace micropixel::firmware {

// Owns Host-level App selection and lifecycle policy. FirmwareApp remains the
// composition root; this controller performs explicit Hall/Foreground state
// transitions after the platform has initialized.
class HostController final {
   public:
    HostController(device::DeviceServices& devices, device::WifiBackend& wifi, host_ui::SystemShell& shell);
    HostController(const HostController&) = delete;
    HostController& operator=(const HostController&) = delete;

    void Run();

   private:
    device::DeviceServices& devices_;
    device::WifiBackend& wifi_;
    host_ui::SystemShell& shell_;
};

}  // namespace micropixel::firmware

#endif
