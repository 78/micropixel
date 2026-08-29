#include "firmware_app.hpp"

#include "device/device_services.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "host/controller/control_dispatcher.hpp"
#include "host/controller/guest_log_buffer.hpp"
#include "host/controller/host_controller.hpp"
#include "host/controller/local/local_control_agent.hpp"
#include "host/controller/remote/remote_control_agent.hpp"
#include "host/time/network_time.hpp"
#include "host/ui/system_shell.hpp"
#include "nvs_flash.h"
#include "platform/platform.hpp"
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

    if (!platform_.Ready()) {
        ESP_LOGE(kTag, "configured board did not publish a complete service set");
        return;
    }
    const platform::PlatformServices& services = platform_.Services();
    static host_ui::SystemShell shell(*services.system_ui);

    auto wifi_result = services.wifi->Initialize();
    if (!wifi_result) {
        ESP_LOGW(kTag, "Wi-Fi is unavailable for this boot: error=%u", static_cast<unsigned>(wifi_result.error()));
    } else {
        const esp_err_t time_error = network_time::Initialize(
            [](void* context) { static_cast<host_ui::SystemShell*>(context)->NotifyTimeStateChanged(); }, &shell);
        if (time_error != ESP_OK) {
            ESP_LOGW(kTag, "Host network time is unavailable for this boot: %s", esp_err_to_name(time_error));
        }
    }

    // These composition-root objects live for the lifetime of the firmware.
    // Keep them out of app_main's bounded stack: RemoteControlAgent owns
    // several fixed-capacity protocol buffers even when remote control is
    // disabled.
    static device::DeviceServices devices(*services.graphics, services.board_info.display, *services.input,
                                          *services.audio, *services.random, *services.devices, *services.sensors,
                                          *services.gpio, *services.haptics, *services.battery);
    static control::GuestLogBuffer guest_logs;
    static control::ControlDispatcher controls(
        [](void* context, const char* app_id) {
            static_cast<control::GuestLogBuffer*>(context)->UpdateAppLifecycle(app_id);
        },
        &guest_logs);
    static remote_control::RemoteControlAgent remote_control(*services.wifi, services.board_info, controls, guest_logs,
                                                             shell.SupportsScreenCapture());
    static local_control::LocalControlAgent local_control(*services.local_control, controls, guest_logs,
                                                          services.board_info, *services.wifi);
    if (!local_control.Start()) {
        ESP_LOGW(kTag, "local control is unavailable for this boot");
    }
    HostController(devices, *services.battery, *services.wifi, *services.power, shell, controls, guest_logs,
                   remote_control, background_executor)
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
