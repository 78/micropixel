#include "firmware_app.hpp"

#include "device/device_services.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "host_controller.hpp"
#include "host_ui/system_shell.hpp"
#include "nvs_flash.h"
#include "platform/platform.hpp"

namespace micropixel::firmware {
namespace {

constexpr char kTag[] = "micropixel_main";

}  // namespace

void FirmwareApp::Run() {
    if (!InitializePlatform()) {
        return;
    }

    auto wifi_result = platform_.wifi().Initialize();
    if (!wifi_result) {
        ESP_LOGW(kTag, "Wi-Fi is unavailable for this boot: error=%u", static_cast<unsigned>(wifi_result.error()));
    }

    device::DeviceServices devices(platform_.graphics(), platform_.input(), platform_.audio(), platform_.random());
    host_ui::SystemShell shell(platform_.system_ui());
    HostController(devices, platform_.battery(), platform_.wifi(), shell).Run();
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
    return {};
}

}  // namespace micropixel::firmware
