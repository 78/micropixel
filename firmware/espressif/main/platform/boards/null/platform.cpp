#include "platform/platform.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/common/unavailable_backends.hpp"
#include "platform/configured_backends.hpp"

namespace micropixel::platform {
namespace {

class NullPowerBackend final : public device::PowerBackend {
   public:
    void SetPowerButtonSink(device::PowerButtonSink, void*) override {}
    void SetPowerOffButtonSink(device::PowerOffButtonSink, void*) override {}
    [[nodiscard]] std::expected<void, device::PowerError> EnterLowPower() override {
        return std::unexpected(device::PowerError::kUnavailable);
    }
    [[noreturn]] void PowerOff() override {
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }
};

class NullLocalControlBackend final : public device::LocalControlBackend {
   public:
    void Bind(device::LocalControlCommandSink, device::LocalControlResponseSource, void*) override {}
    void Unbind(void*) override {}
    void NotifyResponseReady() override {}
};

class NullPlatform final : public Platform {
   public:
    [[nodiscard]] esp_err_t Initialize() override { return ESP_OK; }
    [[nodiscard]] device::GraphicsBackend& graphics() override { return graphics_; }
    [[nodiscard]] device::InputBackend& input() override { return input_; }
    [[nodiscard]] device::AudioBackend& audio() override { return audio_; }
    [[nodiscard]] device::BatteryBackend& battery() override { return battery_; }
    [[nodiscard]] device::RandomBackend& random() override { return ConfiguredRandomBackend(); }
    [[nodiscard]] device::WifiBackend& wifi() override { return wifi_; }
    [[nodiscard]] device::PowerBackend& power() override { return power_; }
    [[nodiscard]] device::LocalControlBackend& local_control() override { return local_control_; }
    [[nodiscard]] device::DeviceCatalogBackend& devices() override { return devices_; }
    [[nodiscard]] device::SensorBackend& sensors() override { return sensors_; }
    [[nodiscard]] device::GpioBackend& gpio() override { return gpio_; }
    [[nodiscard]] device::HapticsBackend& haptics() override { return haptics_; }
    [[nodiscard]] device::HardwareInfoBackend& hardware_info() override { return hardware_info_; }
    [[nodiscard]] host_ui::SystemUiBackend& system_ui() override { return system_ui_; }
    void BindBackgroundExecutor(work::BackgroundExecutor&) override {}

   private:
    common::UnavailableGraphicsBackend graphics_{};
    common::UnavailableAudioBackend audio_{};
    common::UnavailableInputBackend input_{};
    common::UnavailableBatteryBackend battery_{};
    common::UnavailableWifiBackend wifi_{};
    NullPowerBackend power_{};
    NullLocalControlBackend local_control_{};
    common::UnavailableDeviceCatalogBackend devices_{};
    common::UnavailableSensorBackend sensors_{};
    common::UnavailableGpioBackend gpio_{};
    common::UnavailableHapticsBackend haptics_{};
    common::UnavailableHardwareInfoBackend hardware_info_{};
    common::UnavailableSystemUiBackend system_ui_{};
};

}  // namespace

Platform& ConfiguredPlatform() {
    static NullPlatform platform;
    return platform;
}

}  // namespace micropixel::platform
