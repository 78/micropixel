#include "firmware_app.hpp"

#include "device/device_services.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "host_controller.hpp"
#include "host_ui/system_shell.hpp"
#include "local_control/local_control_agent.hpp"
#include "network_time.hpp"
#include "nvs_flash.h"
#include "platform/platform.hpp"
#include "remote_control/remote_control_agent.hpp"
#include "work/background_executor.hpp"

namespace micropixel::firmware {
namespace {

constexpr char kTag[] = "micropixel_main";

}  // namespace

void FirmwareApp::Run() {
    if (!InitializePlatform()) {
        return;
    }

    // Bind shared work before Wi-Fi starts posting events so connection
    // persistence never runs on the system event task.
    static work::BackgroundExecutor background_executor;
    if (!background_executor.valid()) {
        ESP_LOGE(kTag, "shared background executor is unavailable");
        return;
    }
    platform_.BindBackgroundExecutor(background_executor);

    auto wifi_result = platform_.wifi().Initialize();
    if (!wifi_result) {
        ESP_LOGW(kTag, "Wi-Fi is unavailable for this boot: error=%u", static_cast<unsigned>(wifi_result.error()));
    } else {
        const esp_err_t time_error = network_time::Initialize();
        if (time_error != ESP_OK) {
            ESP_LOGW(kTag, "Host network time is unavailable for this boot: %s", esp_err_to_name(time_error));
        }
    }

    // These composition-root objects live for the lifetime of the firmware.
    // Keep them out of app_main's bounded stack: RemoteControlAgent owns
    // several fixed-capacity protocol buffers even when remote control is
    // disabled.
    static device::DeviceServices devices(platform_.graphics(), platform_.input(), platform_.audio(),
                                          platform_.random(), platform_.devices(), platform_.sensors(),
                                          platform_.gpio(), platform_.haptics(), platform_.battery());
    static host_ui::SystemShell shell(platform_.system_ui());
    static remote_control::RemoteControlAgent remote_control(platform_.wifi(), platform_.hardware_info());
    static local_control::LocalControlAgent local_control(platform_.local_control(), remote_control);
    if (!local_control.Start()) {
        ESP_LOGW(kTag, "local control is unavailable for this boot");
    }
    HostController(devices, platform_.battery(), platform_.wifi(), platform_.power(), shell, remote_control,
                   background_executor)
        .Run();
}

std::expected<void, FirmwareApp::StartupError> FirmwareApp::InitializePlatform() {
    esp_err_t nvs_error = nvs_flash_init_partition("runtime_nvs");
    if (nvs_error != ESP_OK) {
        ESP_LOGE(kTag, "runtime_nvs initialization failed without destructive recovery: %s",
                 esp_err_to_name(nvs_error));
        return std::unexpected(StartupError::kNvsInitialization);
    }
    ESP_LOGI(kTag, "runtime_nvs initialized; Guest data preserved across Host OTA/restart");

    if (platform_.Initialize() != ESP_OK) {
        ESP_LOGE(kTag, "configured platform did not initialize");
        return std::unexpected(StartupError::kPlatformInitialization);
    }
    const esp_err_t ota_validation = esp_ota_mark_app_valid_cancel_rollback();
    if (ota_validation == ESP_OK) {
        ESP_LOGI(kTag, "confirmed the running OTA image after platform initialization");
    } else {
        ESP_LOGD(kTag, "running image did not require OTA confirmation: %s", esp_err_to_name(ota_validation));
    }
    return {};
}

}  // namespace micropixel::firmware
