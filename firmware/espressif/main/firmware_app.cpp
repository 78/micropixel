#include "firmware_app.hpp"

#include <cinttypes>

#include "device/device_services.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "host/controller/control_dispatcher.hpp"
#include "host/controller/host_controller.hpp"
#include "host/controller/local/local_control_agent.hpp"
#include "host/controller/remote/remote_control_agent.hpp"
#include "host/logging/system_log_buffer.hpp"
#include "host/time/network_time.hpp"
#include "host/ui/system_shell.hpp"
#include "nvs_flash.h"
#include "platform/platform.hpp"
#include "platform/storage/partition_block_storage.hpp"
#include "runtime/bundle/app_store.hpp"
#include "runtime/bundlefs/bundlefs.hpp"
#include "work/background_executor.hpp"

namespace micropixel::firmware {
namespace {

constexpr char kTag[] = "micropixel_main";
constexpr char kAppStorePartition[] = "app_store";
constexpr auto kAppStoreSubtype = static_cast<esp_partition_subtype_t>(0x40);

// A board-soldered App storage medium belongs to MicroPixel alone, so foreign
// content (typically the vendor's factory FAT image) or a BundleFS formatted
// with another block size is formatted on first use: it only ever holds
// downloaded Apps, which the Store can fetch again. Removable media are the
// user's and are never formatted here. A damaged BundleFS is left untouched in
// both cases; the AppStore reports the state and the System UI offers to
// format it after the user confirms.
void PrepareBoardAppStorage(runtime::BundleFs& store, bool removable) {
    bundlefs_error_t error = store.Mount();
    if (!removable && (error == BUNDLEFS_ERR_NOT_FORMATTED || error == BUNDLEFS_ERR_UNSUPPORTED_FORMAT)) {
        ESP_LOGW(kTag, "board App storage holds %s; formatting it for the App Store",
                 error == BUNDLEFS_ERR_NOT_FORMATTED ? "no BundleFS" : "a BundleFS with another geometry");
        error = store.Format();
        if (error == BUNDLEFS_OK) {
            error = store.Mount();
        }
    }
    if (error != BUNDLEFS_OK) {
        ESP_LOGE(kTag,
                 "external App storage is not ready (BundleFS error %d); downloaded Apps use the NOR app_store until "
                 "it is formatted from System Settings",
                 static_cast<int>(error));
    }
}

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
    logging::SystemLogBuffer& system_logs = logging::SystemLogs();
    static control::ControlDispatcher controls(
        [](void* context, const char* app_id) {
            static_cast<logging::SystemLogBuffer*>(context)->UpdateAppLifecycle(app_id);
        },
        &system_logs);
    static remote_control::RemoteControlAgent remote_control(*services.wifi, services.board_info, controls, system_logs,
                                                             shell.SupportsScreenCapture());
    static local_control::LocalControlAgent local_control(*services.local_control, controls, system_logs,
                                                          services.board_info, *services.wifi);
    if (!local_control.Start()) {
        ESP_LOGW(kTag, "local control is unavailable for this boot");
    }
    // Bundle stores: the NOR app_store partition always hosts Components and
    // factory Apps; a board-published medium (Mosaico NAND) takes downloaded
    // Apps so system storage stays small and mappable.
    static platform::storage::PartitionBlockStorage nor_storage(kAppStorePartition, kAppStoreSubtype);
    static runtime::BundleFs system_store(nor_storage);
    static runtime::BundleFs* external_store = nullptr;
    if (services.app_storage != nullptr) {
        static runtime::BundleFs board_store(*services.app_storage, services.app_storage_block_size);
        if (board_store.data_block_size() == 0U) {
            ESP_LOGW(kTag, "board App storage geometry is unsupported; using the NOR app_store partition");
        } else {
            PrepareBoardAppStorage(board_store, services.app_storage_removable);
            external_store = &board_store;
            ESP_LOGI(kTag, "external App storage: %" PRIu64 " MiB, %" PRIu32 " KiB blocks%s",
                     services.app_storage->geometry().size_bytes / (1024U * 1024U),
                     board_store.data_block_size() / 1024U, services.app_storage_removable ? ", removable" : "");
        }
    }
    if (!nor_storage.present()) {
        ESP_LOGE(kTag, "app_store partition is missing; the App Store is unavailable");
    }
    static runtime::AppStore app_store(system_store, external_store);
    HostController(devices, app_store, *services.battery, *services.wifi, *services.power, shell, controls, system_logs,
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
