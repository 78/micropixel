#include "host_controller.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>

#include "app_controller.hpp"
#include "device/battery.hpp"
#include "device/device_services.hpp"
#include "device/power.hpp"
#include "device/wifi.hpp"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "host_ui/system_settings_store.hpp"
#include "host_ui/system_shell.hpp"
#include "remote_control/remote_control_agent.hpp"
#include "runtime/app_runtime.hpp"
#include "runtime/bundle/app_store.hpp"
#include "runtime/wamr/diagnostics.h"
#include "system_time.hpp"
#include "task_policy.hpp"

namespace micropixel::firmware {
namespace {

constexpr char kTag[] = "micropixel_host";
constexpr TickType_t kCooperativeStopTimeout = pdMS_TO_TICKS(500);
constexpr TickType_t kForcedStopTimeout = pdMS_TO_TICKS(2500);
constexpr int64_t kPerformanceSamplePeriodUs = 1000LL * 1000LL;
constexpr int64_t kBatterySamplePeriodUs = 1000000;
constexpr int64_t kHallStatusSamplePeriodUs = 30LL * 1000LL * 1000LL;
constexpr int64_t kWifiScanRefreshDelayUs = 10LL * 1000LL * 1000LL;
constexpr int64_t kWifiScanRetryDelayUs = 1000LL * 1000LL;
constexpr TickType_t kPowerSuspendTimeout = pdMS_TO_TICKS(500U);
constexpr TickType_t kShutdownRemoteStopTimeout = pdMS_TO_TICKS(500U);
constexpr uint32_t kBrightnessFadeSteps = 12U;
constexpr TickType_t kBrightnessFadeStepDelay = pdMS_TO_TICKS(15U);

const char* PowerErrorText(device::PowerError error) {
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

void RunBasicShutdown(host_ui::SystemShell& shell, device::PowerBackend& power, HostPowerStateMachine& power_state,
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
                        device::PowerBackend& power, HostPowerStateMachine& power_state) {
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
        ESP_LOGE(kTag, "low-power cycle failed: %s", PowerErrorText(sleep_result.error()));
    }
    FadeBrightness(shell, 0U, status_model.brightness_percent);
    if (!power_state.FinishWake()) {
        power_state.RecoverAwake();
    }
    shell.NotifyPowerCycleCompleted();
}

template <typename T>
struct HeapCapsObjectDeleter final {
    void operator()(T* value) const {
        if (value != nullptr) {
            value->~T();
            heap_caps_free(value);
        }
    }
};

template <typename T>
using HeapCapsObjectPtr = std::unique_ptr<T, HeapCapsObjectDeleter<T>>;

template <typename T, typename... Args>
HeapCapsObjectPtr<T> MakePsramObject(Args&&... args) {
    void* memory = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        memory = heap_caps_malloc(sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (memory == nullptr) {
        return {};
    }
    return HeapCapsObjectPtr<T>(new (memory) T(std::forward<Args>(args)...));
}

class CpuUsageSampler final {
   public:
    void Reset() {
        last_sample_us_ = 0U;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        last_idle_time_ = 0U;
#endif
    }

    [[nodiscard]] uint8_t Sample() {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        using Counter = configRUN_TIME_COUNTER_TYPE;
        Counter idle_time = 0U;
#if defined(CONFIG_FREERTOS_SMP) && CONFIG_FREERTOS_SMP
        idle_time = ulTaskGetIdleRunTimeCounter();
#else
        for (BaseType_t core = 0; core < configNUMBER_OF_CORES; ++core) {
            idle_time += ulTaskGetIdleRunTimeCounterForCore(core);
        }
#endif
        const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
        if (last_sample_us_ == 0U) {
            last_sample_us_ = now_us;
            last_idle_time_ = idle_time;
            return 0U;
        }
        const uint64_t elapsed_us = now_us - last_sample_us_;
        const Counter idle_delta = idle_time >= last_idle_time_
                                       ? idle_time - last_idle_time_
                                       : std::numeric_limits<Counter>::max() - last_idle_time_ + idle_time + 1U;
        last_sample_us_ = now_us;
        last_idle_time_ = idle_time;
        const uint64_t available_cpu_us = elapsed_us * configNUMBER_OF_CORES;
        if (available_cpu_us == 0U) {
            return 0U;
        }
        const uint64_t idle_percent = static_cast<uint64_t>(idle_delta) * 100U / available_cpu_us;
        return static_cast<uint8_t>(idle_percent < 100U ? 100U - idle_percent : 0U);
#else
        return 0U;
#endif
    }

   private:
    uint64_t last_sample_us_{};
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    configRUN_TIME_COUNTER_TYPE last_idle_time_{};
#endif
};

using HallCoverMappings = std::array<runtime::LaunchAssetMapping, runtime::kMaxInstalledApps>;

host_ui::HallWifiModel MakeHallWifiModel(const device::WifiSnapshot& wifi) {
    host_ui::HallWifiModel model{.available = wifi.available, .enabled = wifi.enabled, .connected = wifi.connected};
    for (uint32_t index = 0U; index < wifi.saved_network_count; ++index) {
        if (wifi.saved_networks[index].connected) {
            model.ssid = wifi.saved_networks[index].ssid;
            model.rssi = wifi.saved_networks[index].rssi;
            break;
        }
    }
    return model;
}

host_ui::HallBatteryModel MakeHallBatteryModel(const device::BatterySnapshot& battery) {
    const bool charging = battery.external_power_available ? battery.external_power_connected : battery.charging;
    return {.percent = battery.percent, .available = battery.available, .charging = charging};
}

host_ui::HallStatusBarModel MakeHallStatusBarModel(const device::WifiSnapshot& wifi,
                                                   const device::BatterySnapshot& battery) {
    return {.time_text = system_time::FormatBeijingClock(std::time(nullptr)),
            .wifi = MakeHallWifiModel(wifi),
            .battery = MakeHallBatteryModel(battery)};
}

host_ui::HallModel MakeHallModel(const runtime::InstalledAppCatalog& catalog, const device::WifiSnapshot& wifi,
                                 const device::BatterySnapshot& battery, host_ui::HallStatus status,
                                 const runtime::AppRunOutcome* outcome = nullptr, uint32_t detail = 0U,
                                 bool launch_enabled = true, const HallCoverMappings* covers = nullptr,
                                 const std::optional<uint32_t>& suspended_index = std::nullopt,
                                 const host_ui::HallCoverModel* suspended_snapshot = nullptr,
                                 uint64_t transition_trigger_us = 0U, bool firmware_update_available = false) {
    host_ui::HallModel model{.app_count = std::min(catalog.count, host_ui::kMaxHallApps),
                             .status_app_id = outcome != nullptr ? outcome->app_id.data() : nullptr,
                             .status_error_phase = outcome != nullptr && status == host_ui::HallStatus::kAppFailed
                                                       ? runtime::AppSessionErrorPhase(outcome->error)
                                                       : nullptr,
                             .status_error_code = outcome != nullptr && status == host_ui::HallStatus::kAppFailed
                                                      ? runtime::AppSessionErrorCode(outcome->error)
                                                      : nullptr,
                             .status_error_detail = outcome != nullptr && status == host_ui::HallStatus::kAppFailed
                                                        ? outcome->detail.data()
                                                        : nullptr,
                             .status = status,
                             .detail = detail,
                             .status_exit_code = outcome != nullptr ? outcome->exit_code : 0,
                             .status_has_exit_code = outcome != nullptr && outcome->has_exit_code,
                             .launch_enabled = launch_enabled,
                             .status_bar = MakeHallStatusBarModel(wifi, battery),
                             .transition_trigger_us = transition_trigger_us,
                             .firmware_update_available = firmware_update_available};
    for (uint32_t index = 0U; index < catalog.count && index < host_ui::kMaxHallApps; ++index) {
        model.apps[index].app_id = catalog.apps[index].app_id.data();
        model.apps[index].display_name = catalog.apps[index].display_name.data();
        if (covers != nullptr && (*covers)[index].valid()) {
            const auto& asset = (*covers)[index].asset();
            model.apps[index].cover = host_ui::HallCoverModel{
                .data = asset.data,
                .size = asset.size,
                .width = asset.width,
                .height = asset.height,
                .stride = asset.stride,
                .format = asset.format == MICROPIXEL_BUNDLE_FORMAT_PNG ? host_ui::HallCoverFormat::kPng
                                                                       : host_ui::HallCoverFormat::kRgb888,
                .cache_key = (static_cast<uint64_t>(catalog.apps[index].content_id) << 32U) | asset.content_hash};
        }
        if (suspended_index.has_value() && *suspended_index == index) {
            model.apps[index].running = true;
            if (suspended_snapshot != nullptr && suspended_snapshot->data != nullptr) {
                model.apps[index].cover = *suspended_snapshot;
            }
        }
    }
    return model;
}

HallCoverMappings OpenHallCovers(const runtime::InstalledAppCatalog& catalog,
                                 const std::optional<uint32_t>& snapshot_index = std::nullopt) {
    HallCoverMappings covers{};
    const uint32_t visible_count = std::min(catalog.count, host_ui::kMaxHallApps);
    for (uint32_t index = 0U; index < visible_count; ++index) {
        if (snapshot_index.has_value() && *snapshot_index == index) {
            continue;
        }
        auto mapping_result = runtime::LaunchAssetMapping::Open(catalog.apps[index].file);
        if (!mapping_result) {
            ESP_LOGW(kTag, "App Hall cover unavailable: index=%" PRIu32 " app=%s", index,
                     catalog.apps[index].app_id.data());
            continue;
        }
        covers[index] = std::move(*mapping_result);
    }
    return covers;
}

std::expected<runtime::AppRunOutcome, AppControllerError> StopApp(AppController& controller) {
    return controller.Stop(kCooperativeStopTimeout, kForcedStopTimeout);
}

bool ShowHall(host_ui::SystemShell& shell, const host_ui::HallModel& model) {
    auto result = shell.ShowHall(model);
    if (!result) {
        ESP_LOGE(kTag, "failed to render App Hall: error=%u", static_cast<unsigned>(result.error()));
        return false;
    }
    return true;
}

bool RefreshBatteryStatus(host_ui::StatusLayerModel& model, device::BatteryBackend& battery) {
    const device::BatterySnapshot snapshot = battery.Snapshot();
    const bool changed = model.battery_percent != snapshot.percent || model.battery_available != snapshot.available ||
                         model.battery_charging != snapshot.charging ||
                         model.battery_discharging != snapshot.discharging ||
                         model.battery_charging_available != snapshot.charging_available ||
                         model.external_power_connected != snapshot.external_power_connected ||
                         model.external_power_available != snapshot.external_power_available;
    model.battery_percent = snapshot.percent;
    model.battery_available = snapshot.available;
    model.battery_charging = snapshot.charging;
    model.battery_discharging = snapshot.discharging;
    model.battery_charging_available = snapshot.charging_available;
    model.external_power_connected = snapshot.external_power_connected;
    model.external_power_available = snapshot.external_power_available;
    return changed;
}

void RefreshStatusMetrics(host_ui::StatusLayerModel& model, const runtime::InstalledAppCatalog& catalog,
                          device::BatteryBackend& battery) {
    const size_t memory_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    const size_t memory_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    model.memory_total_kib = static_cast<uint32_t>(memory_total / 1024U);
    model.memory_used_kib = static_cast<uint32_t>((memory_total - memory_free) / 1024U);

    constexpr uint32_t kSramCapabilities = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t sram_total = heap_caps_get_total_size(kSramCapabilities);
    const size_t sram_free = heap_caps_get_free_size(kSramCapabilities);
    model.sram_total_kib = static_cast<uint32_t>(sram_total / 1024U);
    model.sram_used_kib = static_cast<uint32_t>((sram_total - sram_free) / 1024U);

    model.storage_total_kib = catalog.store_total_bytes / 1024U;
    model.storage_used_kib = catalog.store_used_bytes / 1024U;

    (void)RefreshBatteryStatus(model, battery);
}

void RefreshWifiStatus(host_ui::StatusLayerModel& model, const device::WifiSnapshot& snapshot) {
    model.wifi_available = snapshot.available;
    model.wifi_enabled = snapshot.enabled;
    model.wifi_connected = snapshot.connected;
    model.wifi_connecting = snapshot.connection_state == device::WifiConnectionState::kConnecting;
}

host_ui::WifiBand HostWifiBand(device::WifiBand band) {
    switch (band) {
        case device::WifiBand::k2_4Ghz:
            return host_ui::WifiBand::k2_4Ghz;
        case device::WifiBand::k5Ghz:
            return host_ui::WifiBand::k5Ghz;
        case device::WifiBand::kUnknown:
        default:
            return host_ui::WifiBand::kUnknown;
    }
}

host_ui::WifiConnectionState HostWifiConnectionState(device::WifiConnectionState state) {
    switch (state) {
        case device::WifiConnectionState::kConnecting:
            return host_ui::WifiConnectionState::kConnecting;
        case device::WifiConnectionState::kConnected:
            return host_ui::WifiConnectionState::kConnected;
        case device::WifiConnectionState::kAuthenticationFailed:
            return host_ui::WifiConnectionState::kAuthenticationFailed;
        case device::WifiConnectionState::kAuthenticationTimedOut:
            return host_ui::WifiConnectionState::kAuthenticationTimedOut;
        case device::WifiConnectionState::kHandshakeTimedOut:
            return host_ui::WifiConnectionState::kHandshakeTimedOut;
        case device::WifiConnectionState::kNetworkNotFound:
            return host_ui::WifiConnectionState::kNetworkNotFound;
        case device::WifiConnectionState::kFailed:
            return host_ui::WifiConnectionState::kFailed;
        case device::WifiConnectionState::kDisconnected:
        default:
            return host_ui::WifiConnectionState::kDisconnected;
    }
}

host_ui::WifiNetworkModel MakeWifiNetworkModel(const device::WifiNetwork& network) {
    return host_ui::WifiNetworkModel{
        .ssid = network.ssid,
        .rssi = network.rssi,
        .channel = network.channel,
        .band = HostWifiBand(network.band),
        .secured = network.security == device::WifiSecurity::kSecured,
        .saved = network.saved,
        .connected = network.connected,
    };
}

host_ui::WifiSettingsModel MakeWifiSettingsModel(const device::WifiSnapshot& snapshot) {
    host_ui::WifiSettingsModel model{
        .saved_network_count = snapshot.saved_network_count,
        .available_network_count = snapshot.available_network_count,
        .available = snapshot.available,
        .enabled = snapshot.enabled,
        .connected = snapshot.connected,
        .scanning = snapshot.scanning,
        .connection_state = HostWifiConnectionState(snapshot.connection_state),
    };
    for (uint32_t index = 0U; index < snapshot.saved_network_count && index < model.saved_networks.size(); ++index) {
        model.saved_networks[index] = MakeWifiNetworkModel(snapshot.saved_networks[index]);
    }
    for (uint32_t index = 0U; index < snapshot.available_network_count && index < model.available_networks.size();
         ++index) {
        model.available_networks[index] = MakeWifiNetworkModel(snapshot.available_networks[index]);
    }
    return model;
}

bool SameWifiNetwork(const device::WifiNetwork& left, const device::WifiNetwork& right) {
    return left.ssid == right.ssid && left.rssi == right.rssi && left.channel == right.channel &&
           left.band == right.band && left.security == right.security && left.saved == right.saved &&
           left.connected == right.connected;
}

bool SameWifiSnapshot(const device::WifiSnapshot& left, const device::WifiSnapshot& right) {
    if (left.saved_network_count != right.saved_network_count ||
        left.available_network_count != right.available_network_count || left.available != right.available ||
        left.enabled != right.enabled || left.connected != right.connected || left.scanning != right.scanning) {
        return false;
    }
    if (left.connection_state != right.connection_state) {
        return false;
    }
    for (uint32_t index = 0U; index < left.saved_network_count; ++index) {
        if (!SameWifiNetwork(left.saved_networks[index], right.saved_networks[index])) {
            return false;
        }
    }
    for (uint32_t index = 0U; index < left.available_network_count; ++index) {
        if (!SameWifiNetwork(left.available_networks[index], right.available_networks[index])) {
            return false;
        }
    }
    return true;
}

host_ui::SystemMenuModel MakeSystemMenuModel(const host_ui::StatusLayerModel& status,
                                             const runtime::InstalledAppCatalog& catalog,
                                             const host_ui::RemoteControlModel& remote_control) {
    return host_ui::SystemMenuModel{
        .language = "English",
        .installed_app_count = catalog.count,
        .auto_sleep_timeout_minutes = status.auto_sleep_timeout_minutes,
        .wifi_available = status.wifi_available,
        .wifi_enabled = status.wifi_enabled,
        .wifi_connected = status.wifi_connected,
        .wifi_connecting = status.wifi_connecting,
        .remote_control_enabled = remote_control.enabled,
        .remote_control_connected =
            remote_control.connection_state == host_ui::RemoteControlConnectionState::kConnected,
        .firmware_update_available = remote_control.firmware_update_available,
        .latest_firmware_version = remote_control.latest_firmware_version,
        .firmware_update_message = remote_control.firmware_update_message,
        .firmware_update_state = remote_control.firmware_update_state,
    };
}

bool SameRemoteControlModel(const host_ui::RemoteControlModel& left, const host_ui::RemoteControlModel& right) {
    return left.service == right.service && left.device_id == right.device_id &&
           left.pairing_code == right.pairing_code && left.status_message == right.status_message &&
           left.pairing_expires_seconds == right.pairing_expires_seconds &&
           left.connection_state == right.connection_state && left.enabled == right.enabled &&
           left.pairing_code_pending == right.pairing_code_pending &&
           left.pairing_code_available == right.pairing_code_available &&
           left.latest_firmware_version == right.latest_firmware_version &&
           left.firmware_update_message == right.firmware_update_message &&
           left.firmware_size_bytes == right.firmware_size_bytes &&
           left.firmware_processed_bytes == right.firmware_processed_bytes &&
           left.firmware_progress_percent == right.firmware_progress_percent &&
           left.firmware_update_state == right.firmware_update_state &&
           left.firmware_update_available == right.firmware_update_available &&
           left.firmware_update_installable == right.firmware_update_installable;
}

bool SameFirmwareUpdate(const host_ui::RemoteControlModel& left, const host_ui::RemoteControlModel& right) {
    return left.latest_firmware_version == right.latest_firmware_version &&
           left.firmware_update_message == right.firmware_update_message &&
           left.firmware_size_bytes == right.firmware_size_bytes &&
           left.firmware_processed_bytes == right.firmware_processed_bytes &&
           left.firmware_progress_percent == right.firmware_progress_percent &&
           left.firmware_update_state == right.firmware_update_state &&
           left.firmware_update_available == right.firmware_update_available &&
           left.firmware_update_installable == right.firmware_update_installable;
}

bool FirmwareUpdateInProgress(host_ui::FirmwareUpdateState state) {
    return state == host_ui::FirmwareUpdateState::kDownloading || state == host_ui::FirmwareUpdateState::kVerifying ||
           state == host_ui::FirmwareUpdateState::kInstalling;
}

TickType_t DeadlineWaitTimeout(int64_t deadline_us) {
    if (deadline_us == 0) {
        return portMAX_DELAY;
    }
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0U;
    }
    const uint64_t remaining_ms = (static_cast<uint64_t>(remaining_us) + 999U) / 1000U;
    const TickType_t timeout = pdMS_TO_TICKS(remaining_ms);
    return timeout == 0U ? 1U : timeout;
}

template <size_t Capacity>
void CopySystemInformationText(std::array<char, Capacity>& destination, const char* source) {
    destination.fill('\0');
    if (source != nullptr) {
        std::snprintf(destination.data(), destination.size(), "%s", source);
    }
}

host_ui::MemoryStatisticsModel ReadMemoryStatistics(uint32_t capabilities) {
    return host_ui::MemoryStatisticsModel{
        .total_kib = static_cast<uint32_t>(heap_caps_get_total_size(capabilities) / 1024U),
        .free_kib = static_cast<uint32_t>(heap_caps_get_free_size(capabilities) / 1024U),
        .minimum_free_kib = static_cast<uint32_t>(heap_caps_get_minimum_free_size(capabilities) / 1024U),
        .largest_free_block_kib = static_cast<uint32_t>(heap_caps_get_largest_free_block(capabilities) / 1024U),
    };
}

const char* ResetReasonText(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:
            return "Power on";
        case ESP_RST_EXT:
            return "External reset";
        case ESP_RST_SW:
            return "Software reset";
        case ESP_RST_PANIC:
            return "Panic";
        case ESP_RST_INT_WDT:
            return "Interrupt watchdog";
        case ESP_RST_TASK_WDT:
            return "Task watchdog";
        case ESP_RST_WDT:
            return "Watchdog";
        case ESP_RST_DEEPSLEEP:
            return "Deep sleep wake";
        case ESP_RST_BROWNOUT:
            return "Brownout";
        case ESP_RST_SDIO:
            return "SDIO reset";
        case ESP_RST_USB:
            return "USB reset";
        case ESP_RST_JTAG:
            return "JTAG reset";
        case ESP_RST_EFUSE:
            return "eFuse error";
        case ESP_RST_PWR_GLITCH:
            return "Power glitch";
        case ESP_RST_CPU_LOCKUP:
            return "CPU lockup";
        case ESP_RST_UNKNOWN:
        default:
            return "Unknown";
    }
}

host_ui::SystemInformationModel MakeSystemInformationModel(const host_ui::RemoteControlModel& remote_control,
                                                           bool firmware_update_view = false) {
    host_ui::SystemInformationModel model{};
    const esp_app_desc_t* description = esp_app_get_description();
    if (description != nullptr) {
        CopySystemInformationText(model.firmware_version, description->version);
        CopySystemInformationText(model.build_date, description->date);
        CopySystemInformationText(model.build_time, description->time);
        CopySystemInformationText(model.idf_version, description->idf_ver);
    }
    char elf_sha[65]{};
    (void)esp_app_get_elf_sha256(elf_sha, sizeof(elf_sha));
    std::snprintf(model.build_id.data(), model.build_id.size(), "%.8s", elf_sha);

    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    std::snprintf(model.host_chip.data(), model.host_chip.size(), "ESP32-P4 Rev %u.%u",
                  static_cast<unsigned>(chip.revision / 100U), static_cast<unsigned>(chip.revision % 100U));
    std::snprintf(model.cpu.data(), model.cpu.size(), "%u Core%s / %u MHz", static_cast<unsigned>(chip.cores),
                  chip.cores == 1U ? "" : "s", static_cast<unsigned>(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ));
    CopySystemInformationText(model.wifi_coprocessor, "ESP32-C5");
    std::array<uint8_t, 6> wifi_mac{};
    if (esp_wifi_get_mac(WIFI_IF_STA, wifi_mac.data()) == ESP_OK) {
        std::snprintf(model.wifi_mac.data(), model.wifi_mac.size(), "%02X:%02X:%02X:%02X:%02X:%02X",
                      static_cast<unsigned>(wifi_mac[0]), static_cast<unsigned>(wifi_mac[1]),
                      static_cast<unsigned>(wifi_mac[2]), static_cast<unsigned>(wifi_mac[3]),
                      static_cast<unsigned>(wifi_mac[4]), static_cast<unsigned>(wifi_mac[5]));
    } else {
        CopySystemInformationText(model.wifi_mac, "Unknown");
    }

    uint32_t flash_bytes = 0U;
    if (esp_flash_get_size(nullptr, &flash_bytes) == ESP_OK) {
        std::snprintf(model.flash_capacity.data(), model.flash_capacity.size(), "%" PRIu32 " MB",
                      flash_bytes / (1024U * 1024U));
    } else {
        CopySystemInformationText(model.flash_capacity, "Unknown");
    }

    CopySystemInformationText(model.panel, "NV3051F");
    CopySystemInformationText(model.display_interface, "MIPI-DSI / 2 lanes");
    CopySystemInformationText(model.resolution, "720 x 720 / RGB888");
    CopySystemInformationText(model.touch_controller, "GT911");
    CopySystemInformationText(model.last_reset, ResetReasonText(esp_reset_reason()));

    const uint64_t uptime_seconds = static_cast<uint64_t>(esp_timer_get_time()) / 1000000U;
    const uint64_t days = uptime_seconds / 86400U;
    const uint64_t hours = (uptime_seconds / 3600U) % 24U;
    const uint64_t minutes = (uptime_seconds / 60U) % 60U;
    if (days != 0U) {
        std::snprintf(model.uptime.data(), model.uptime.size(), "%" PRIu64 "d %" PRIu64 "h %" PRIu64 "m", days, hours,
                      minutes);
    } else {
        std::snprintf(model.uptime.data(), model.uptime.size(), "%" PRIu64 "h %" PRIu64 "m", hours, minutes);
    }

    constexpr uint32_t kInternalSramCapabilities = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t kPsramCapabilities = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    model.internal_sram = ReadMemoryStatistics(kInternalSramCapabilities);
    model.psram = ReadMemoryStatistics(kPsramCapabilities);
    model.latest_firmware_version = remote_control.latest_firmware_version;
    model.firmware_update_message = remote_control.firmware_update_message;
    model.firmware_size_bytes = remote_control.firmware_size_bytes;
    model.firmware_processed_bytes = remote_control.firmware_processed_bytes;
    model.firmware_progress_percent = remote_control.firmware_progress_percent;
    model.firmware_update_state = remote_control.firmware_update_state;
    model.firmware_update_available = remote_control.firmware_update_available;
    model.firmware_update_installable = remote_control.firmware_update_installable;
    model.firmware_update_view = firmware_update_view;
    return model;
}

host_ui::AppManagementModel MakeAppManagementModel(const runtime::InstalledAppCatalog& catalog, bool launch_available,
                                                   bool uninstall_available) {
    host_ui::AppManagementModel model{.app_count = std::min(catalog.count, host_ui::kMaxHallApps),
                                      .launch_available = launch_available,
                                      .uninstall_available = uninstall_available};
    model.storage_total_kib = catalog.store_total_bytes / 1024U;
    model.storage_used_kib = catalog.store_used_bytes / 1024U;
    for (uint32_t index = 0U; index < model.app_count; ++index) {
        const runtime::InstalledApp& source = catalog.apps[index];
        model.apps[index] = host_ui::InstalledAppModel{
            .app_id = source.app_id.data(),
            .display_name = source.display_name.data(),
            .bundle_size_kib = source.bundle_size / 1024U,
        };
    }
    return model;
}

remote_control::RemoteControlCatalogSnapshot MakeRemoteControlCatalog(const runtime::InstalledAppCatalog& catalog) {
    remote_control::RemoteControlCatalogSnapshot snapshot{};
    snapshot.count = std::min(catalog.count, static_cast<uint32_t>(snapshot.apps.size()));
    snapshot.store_total_bytes = catalog.store_total_bytes;
    snapshot.store_used_bytes = catalog.store_used_bytes;
    for (uint32_t index = 0U; index < snapshot.count; ++index) {
        const runtime::InstalledApp& source = catalog.apps[index];
        auto& destination = snapshot.apps[index];
        std::snprintf(destination.app_id.data(), destination.app_id.size(), "%s", source.app_id.data());
        std::snprintf(destination.display_name.data(), destination.display_name.size(), "%s",
                      source.display_name.data());
        destination.bundle_size = source.bundle_size;
    }
    return snapshot;
}

// Keep the 50-entry catalog snapshot out of long-lived caller stack frames.
// In particular, HostController::Run() does not return while the product is
// running, so an inlined/by-value snapshot temporary there would permanently
// consume roughly 7 KiB of the main task stack.
[[gnu::noinline]] void UpdateRemoteControlCatalog(remote_control::RemoteControlAgent& remote_control,
                                                  const runtime::InstalledAppCatalog& catalog) {
    remote_control.UpdateInstalledApps(MakeRemoteControlCatalog(catalog));
}

const char* RemoteLifecycleText(AppLifecycleState state) {
    switch (state) {
        case AppLifecycleState::kStarting:
            return "starting";
        case AppLifecycleState::kForeground:
            return "foreground";
        case AppLifecycleState::kSuspending:
            return "suspending";
        case AppLifecycleState::kSuspended:
            return "suspended";
        case AppLifecycleState::kResuming:
            return "resuming";
        case AppLifecycleState::kStopping:
            return "stopping";
        case AppLifecycleState::kNotRunning:
        default:
            return "not_running";
    }
}

const char* AppStoreErrorText(runtime::AppStoreError error) {
    switch (error) {
        case runtime::AppStoreError::kUnavailable:
            return "app_store_unavailable";
        case runtime::AppStoreError::kCatalogCorrupt:
            return "app_catalog_corrupt";
        case runtime::AppStoreError::kInvalidPackage:
            return "package_invalid";
        case runtime::AppStoreError::kHashMismatch:
            return "package_hash_mismatch";
        case runtime::AppStoreError::kAppIdMismatch:
            return "package_app_id_mismatch";
        case runtime::AppStoreError::kCatalogFull:
            return "app_catalog_full";
        case runtime::AppStoreError::kNoSpace:
            return "app_store_full";
        case runtime::AppStoreError::kFlashWrite:
            return "app_store_write_failed";
        case runtime::AppStoreError::kCommitFailed:
            return "app_catalog_commit_failed";
        case runtime::AppStoreError::kNotFound:
        default:
            return "app_not_found";
    }
}

const char* AppControllerErrorText(AppControllerError error) {
    switch (error) {
        case AppControllerError::kUnavailable:
            return "app_runtime_unavailable";
        case AppControllerError::kAppAlreadyActive:
            return "app_already_active";
        case AppControllerError::kPthreadConfiguration:
            return "app_thread_configuration_failed";
        case AppControllerError::kPthreadAttributes:
            return "app_thread_attributes_failed";
        case AppControllerError::kThreadCreation:
            return "app_thread_creation_failed";
        case AppControllerError::kThreadJoin:
            return "app_thread_join_failed";
        case AppControllerError::kInvalidState:
            return "app_invalid_lifecycle_state";
        case AppControllerError::kSuspendTimeout:
            return "app_suspend_timeout";
        case AppControllerError::kResumeFailed:
            return "app_resume_failed";
        case AppControllerError::kTerminationTimeout:
        default:
            return "app_termination_timeout";
    }
}

void AddAppDiagnostic(remote_control::RemoteControlHostResult& result, const runtime::AppRunOutcome& outcome) {
    result.has_diagnostic = true;
    std::snprintf(result.diagnostic.app_id.data(), result.diagnostic.app_id.size(), "%s", outcome.app_id.data());
    std::snprintf(result.diagnostic.phase.data(), result.diagnostic.phase.size(), "%s",
                  runtime::AppSessionErrorPhase(outcome.error));
    std::snprintf(result.diagnostic.code.data(), result.diagnostic.code.size(), "%s",
                  runtime::AppSessionErrorCode(outcome.error));
    std::snprintf(result.diagnostic.detail.data(), result.diagnostic.detail.size(), "%s", outcome.detail.data());
    result.diagnostic.exit_code = outcome.exit_code;
    result.diagnostic.has_exit_code = outcome.has_exit_code;
}

struct RemoteCommandPump final {
    bool (*poll)(void*){};
    bool (*requires_periodic_poll)(void*){};
    void* context{};
    bool unwind_requested{};

    [[nodiscard]] bool Process() {
        if (!unwind_requested && poll != nullptr) {
            unwind_requested = poll(context);
        }
        return unwind_requested;
    }

    [[nodiscard]] bool RequiresPeriodicPoll() const {
        return requires_periodic_poll != nullptr && requires_periodic_poll(context);
    }
};

struct AppManagementUninstallHandler final {
    bool (*uninstall)(void*, uint32_t){};
    void* context{};
    bool available{};

    [[nodiscard]] bool Uninstall(uint32_t app_index) const {
        return available && uninstall != nullptr && uninstall(context, app_index);
    }
};

TickType_t RemoteAwareTimeout(TickType_t timeout, const RemoteCommandPump* command_pump) {
    constexpr TickType_t kMaximumWait = pdMS_TO_TICKS(250U);
    return command_pump != nullptr && command_pump->RequiresPeriodicPoll() && timeout > kMaximumWait ? kMaximumWait
                                                                                                     : timeout;
}

bool RunWifiSettings(host_ui::SystemShell& shell, device::WifiBackend& wifi, host_ui::StatusLayerModel& status_model,
                     RemoteCommandPump* command_pump);

bool RunStatusLayer(host_ui::SystemShell& shell, AppController* controller, device::BatteryBackend& battery,
                    device::WifiBackend& wifi, host_ui::StatusLayerModel& model,
                    const runtime::InstalledAppCatalog& catalog, host_ui::SystemSettingsStore& settings_store,
                    RemoteCommandPump* command_pump, uint64_t trigger_timestamp_us) {
    if (controller != nullptr) {
        auto suspend_result = controller->Suspend(pdMS_TO_TICKS(500));
        if (!suspend_result) {
            ESP_LOGE(kTag, "failed to suspend App for status layer: error=%u",
                     static_cast<unsigned>(suspend_result.error()));
            return false;
        }
        shell.StopWatchingGuestActions();
    }
    RefreshStatusMetrics(model, catalog, battery);
    RefreshWifiStatus(model, wifi.Snapshot());
    auto show_result = shell.ShowStatusLayer(model, trigger_timestamp_us);
    if (!show_result) {
        ESP_LOGE(kTag, "failed to show status layer: error=%u", static_cast<unsigned>(show_result.error()));
        if (controller == nullptr) {
            return false;
        }
        auto resume_result = controller->Resume();
        if (resume_result) {
            shell.WatchGuestActions();
        }
        return resume_result.has_value();
    }
    CpuUsageSampler paused_cpu_sampler;
    paused_cpu_sampler.Reset();
    (void)paused_cpu_sampler.Sample();
    int64_t next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
    int64_t next_battery_sample_us = esp_timer_get_time() + kBatterySamplePeriodUs;
    bool close = false;
    bool open_wifi_settings = false;
    bool settings_changed = false;
    uint64_t close_trigger_timestamp_us = 0U;
    while (!close) {
        const TickType_t timeout =
            model.performance_overlay_enabled ? DeadlineWaitTimeout(next_performance_sample_us) : pdMS_TO_TICKS(250);
        const auto action = shell.PollAction(RemoteAwareTimeout(timeout, command_pump));
        if (command_pump != nullptr && command_pump->Process()) {
            close = true;
            continue;
        }
        if (!action.has_value()) {
            const int64_t now_us = esp_timer_get_time();
            if (now_us >= next_battery_sample_us) {
                if (RefreshBatteryStatus(model, battery)) {
                    shell.UpdateStatusLayer(model);
                }
                next_battery_sample_us = now_us + kBatterySamplePeriodUs;
            }
            if (model.performance_overlay_enabled && now_us >= next_performance_sample_us) {
                shell.UpdatePerformanceOverlay(true, paused_cpu_sampler.Sample());
                next_performance_sample_us = now_us + kPerformanceSamplePeriodUs;
            }
            continue;
        }
        bool controls_changed = false;
        switch (action->type) {
            case host_ui::SystemUiActionType::kCloseStatusLayer:
            case host_ui::SystemUiActionType::kSuspendToHall:
                close_trigger_timestamp_us = action->timestamp_us;
                close = true;
                break;
            case host_ui::SystemUiActionType::kSetBrightness: {
                const uint8_t brightness_percent = static_cast<uint8_t>(action->value <= 100U ? action->value : 100U);
                settings_changed = settings_changed || model.brightness_percent != brightness_percent;
                model.brightness_percent = brightness_percent;
                shell.ApplyBrightness(model.brightness_percent);
                break;
            }
            case host_ui::SystemUiActionType::kSetVolume: {
                const uint8_t volume_percent = static_cast<uint8_t>(action->value <= 100U ? action->value : 100U);
                settings_changed = settings_changed || model.volume_percent != volume_percent;
                model.volume_percent = volume_percent;
                shell.ApplyVolume(model.volume_percent);
                break;
            }
            case host_ui::SystemUiActionType::kTogglePerformanceOverlay:
                model.performance_overlay_enabled = !model.performance_overlay_enabled;
                settings_changed = true;
                if (model.performance_overlay_enabled) {
                    paused_cpu_sampler.Reset();
                    (void)paused_cpu_sampler.Sample();
                    next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                    shell.UpdatePerformanceOverlay(true, 0U);
                } else {
                    shell.UpdatePerformanceOverlay(false, 0U);
                }
                controls_changed = true;
                break;
            case host_ui::SystemUiActionType::kWifiStateChanged:
                RefreshWifiStatus(model, wifi.Snapshot());
                controls_changed = true;
                break;
            case host_ui::SystemUiActionType::kBatteryStateChanged:
                controls_changed = RefreshBatteryStatus(model, battery);
                next_battery_sample_us = esp_timer_get_time() + kBatterySamplePeriodUs;
                break;
            case host_ui::SystemUiActionType::kOpenWifiSettings:
                open_wifi_settings = true;
                close = true;
                break;
            case host_ui::SystemUiActionType::kOpenStatusLayer:
                break;
            default:
                ESP_LOGW(kTag, "ignored action=%u while status layer is visible", static_cast<unsigned>(action->type));
                break;
        }
        if (!close && controls_changed) {
            shell.UpdateStatusLayer(model);
        }
        const int64_t now_us = esp_timer_get_time();
        if (!close && model.performance_overlay_enabled && now_us >= next_performance_sample_us) {
            shell.UpdatePerformanceOverlay(true, paused_cpu_sampler.Sample());
            next_performance_sample_us = now_us + kPerformanceSamplePeriodUs;
        }
    }

    shell.LeaveStatusLayer(close_trigger_timestamp_us);
    if (settings_changed && settings_store.ready() && !settings_store.Save(model)) {
        ESP_LOGW(kTag, "Host settings changed but could not be persisted");
    }
    const bool wifi_settings_ok = !open_wifi_settings || RunWifiSettings(shell, wifi, model, command_pump);
    if (controller == nullptr) {
        return wifi_settings_ok;
    }
    if (shell.PowerTransitionRequested()) {
        return wifi_settings_ok;
    }
    auto resume_result = controller->Resume();
    if (!resume_result) {
        ESP_LOGE(kTag, "failed to resume App after status layer: error=%u",
                 static_cast<unsigned>(resume_result.error()));
        (void)controller->RequestStop();
        return false;
    }
    shell.WatchGuestActions();
    return wifi_settings_ok;
}

bool RunWifiSettings(host_ui::SystemShell& shell, device::WifiBackend& wifi, host_ui::StatusLayerModel& status_model,
                     RemoteCommandPump* command_pump) {
    device::WifiSnapshot snapshot = wifi.Snapshot();
    RefreshWifiStatus(status_model, snapshot);
    auto show_result = shell.ShowWifiSettings(MakeWifiSettingsModel(snapshot));
    if (!show_result) {
        ESP_LOGE(kTag, "failed to show Wi-Fi settings: error=%u", static_cast<unsigned>(show_result.error()));
        return false;
    }

    bool scan_view = false;
    bool scan_cycle_active = false;
    int64_t next_scan_request_us = 0;
    for (;;) {
        const TickType_t timeout =
            scan_view && snapshot.enabled ? DeadlineWaitTimeout(next_scan_request_us) : portMAX_DELAY;
        const auto action = shell.PollAction(RemoteAwareTimeout(timeout, command_pump));
        if (command_pump != nullptr && command_pump->Process()) {
            snapshot = wifi.Snapshot();
            RefreshWifiStatus(status_model, snapshot);
            shell.LeaveWifiSettings();
            return true;
        }
        bool refresh_snapshot = false;
        if (action.has_value()) {
            std::expected<void, device::WifiError> operation{};
            switch (action->type) {
                case host_ui::SystemUiActionType::kCloseWifiSettings:
                case host_ui::SystemUiActionType::kSuspendToHall:
                    snapshot = wifi.Snapshot();
                    RefreshWifiStatus(status_model, snapshot);
                    shell.LeaveWifiSettings();
                    return true;
                case host_ui::SystemUiActionType::kOpenWifiNetworkScan:
                    scan_view = true;
                    scan_cycle_active = snapshot.scanning;
                    next_scan_request_us = 0;
                    operation = wifi.RequestScan();
                    if (!operation) {
                        next_scan_request_us = esp_timer_get_time() + kWifiScanRetryDelayUs;
                    }
                    break;
                case host_ui::SystemUiActionType::kCloseWifiNetworkScan:
                    scan_view = false;
                    scan_cycle_active = false;
                    next_scan_request_us = 0;
                    break;
                case host_ui::SystemUiActionType::kSetWifiEnabled:
                    operation = wifi.SetEnabled(action->value != 0U);
                    break;
                case host_ui::SystemUiActionType::kConnectSavedWifi:
                    operation = wifi.ConnectSaved(action->text.data());
                    break;
                case host_ui::SystemUiActionType::kConnectNewWifi:
                    operation = wifi.Connect(action->text.data(), action->secret.data());
                    break;
                case host_ui::SystemUiActionType::kDisconnectWifi:
                    operation = wifi.Disconnect();
                    break;
                case host_ui::SystemUiActionType::kForgetWifi:
                    operation = wifi.Forget(action->text.data());
                    break;
                case host_ui::SystemUiActionType::kWifiStateChanged:
                    refresh_snapshot = true;
                    break;
                default:
                    ESP_LOGW(kTag, "ignored action=%u while Wi-Fi settings are visible",
                             static_cast<unsigned>(action->type));
                    break;
            }
            if (!operation) {
                ESP_LOGW(kTag, "Wi-Fi action=%u failed: error=%u", static_cast<unsigned>(action->type),
                         static_cast<unsigned>(operation.error()));
            }
        }

        if (refresh_snapshot) {
            device::WifiSnapshot refreshed = wifi.Snapshot();
            if (!SameWifiSnapshot(snapshot, refreshed)) {
                snapshot = refreshed;
                RefreshWifiStatus(status_model, snapshot);
                shell.UpdateWifiSettings(MakeWifiSettingsModel(snapshot));
            }
        }
        if (!scan_view || !snapshot.enabled) {
            continue;
        }
        if (snapshot.scanning) {
            scan_cycle_active = true;
            continue;
        }
        const int64_t now_us = esp_timer_get_time();
        if (scan_cycle_active) {
            scan_cycle_active = false;
            next_scan_request_us = now_us + kWifiScanRefreshDelayUs;
            continue;
        }
        if (next_scan_request_us != 0 && now_us >= next_scan_request_us &&
            snapshot.connection_state == device::WifiConnectionState::kConnecting) {
            next_scan_request_us = now_us + kWifiScanRetryDelayUs;
            continue;
        }
        if (next_scan_request_us != 0 && now_us >= next_scan_request_us) {
            if (const auto scan_result = wifi.RequestScan(); !scan_result) {
                ESP_LOGW(kTag, "periodic Wi-Fi scan failed: error=%u", static_cast<unsigned>(scan_result.error()));
                next_scan_request_us = now_us + kWifiScanRetryDelayUs;
            } else {
                next_scan_request_us = 0;
            }
        }
    }
}

bool RunFirmwareUpdate(host_ui::SystemShell& shell, remote_control::RemoteControlAgent& remote_control,
                       RemoteCommandPump* command_pump) {
    host_ui::RemoteControlModel remote_model = remote_control.Snapshot();
    const auto show_result = shell.ShowSystemInformation(MakeSystemInformationModel(remote_model, true));
    if (!show_result) {
        ESP_LOGE(kTag, "failed to show Firmware Update: error=%u", static_cast<unsigned>(show_result.error()));
        return false;
    }
    bool request_pending = FirmwareUpdateInProgress(remote_model.firmware_update_state);
    for (;;) {
        const auto action = shell.PollAction(pdMS_TO_TICKS(100U));
        const host_ui::RemoteControlModel latest = remote_control.Snapshot();
        if (!SameFirmwareUpdate(remote_model, latest)) {
            remote_model = latest;
            request_pending = FirmwareUpdateInProgress(remote_model.firmware_update_state);
            shell.UpdateSystemInformation(MakeSystemInformationModel(remote_model, true));
        }
        if (shell.PowerOffRequested()) {
            if (request_pending || FirmwareUpdateInProgress(remote_model.firmware_update_state)) {
                (void)shell.ConsumePowerOffRequested();
                ESP_LOGW(kTag, "power off ignored while firmware update is in progress");
                continue;
            }
            if (command_pump != nullptr) {
                (void)command_pump->Process();
            }
            shell.LeaveSystemInformation();
            return true;
        }
        if (shell.PowerButtonPressed()) {
            if (request_pending || FirmwareUpdateInProgress(remote_model.firmware_update_state)) {
                (void)shell.ConsumePowerButtonPressed();
                ESP_LOGW(kTag, "power button ignored while firmware update is in progress");
                continue;
            }
            if (command_pump != nullptr) {
                (void)command_pump->Process();
            }
            shell.LeaveSystemInformation();
            return true;
        }
        if (!FirmwareUpdateInProgress(remote_model.firmware_update_state) && command_pump != nullptr &&
            command_pump->Process()) {
            shell.LeaveSystemInformation();
            return true;
        }
        if (!action.has_value()) {
            continue;
        }
        if (action->type == host_ui::SystemUiActionType::kCloseSystemInformation ||
            action->type == host_ui::SystemUiActionType::kSuspendToHall) {
            if (request_pending || FirmwareUpdateInProgress(remote_model.firmware_update_state)) {
                continue;
            }
            shell.LeaveSystemInformation();
            return true;
        }
        if (action->type == host_ui::SystemUiActionType::kInstallFirmwareUpdate) {
            if (FirmwareUpdateInProgress(remote_model.firmware_update_state)) {
                continue;
            }
            request_pending = remote_control.RequestFirmwareUpdate();
            if (!request_pending) {
                ESP_LOGW(kTag, "firmware update request was rejected");
            }
            continue;
        }
        ESP_LOGW(kTag, "ignored action=%u while Firmware Update is visible", static_cast<unsigned>(action->type));
    }
}

bool RunSystemInformation(host_ui::SystemShell& shell, remote_control::RemoteControlAgent& remote_control,
                          RemoteCommandPump* command_pump) {
    host_ui::RemoteControlModel remote_model = remote_control.Snapshot();
    const auto show_result = shell.ShowSystemInformation(MakeSystemInformationModel(remote_model));
    if (!show_result) {
        ESP_LOGE(kTag, "failed to show System Information: error=%u", static_cast<unsigned>(show_result.error()));
        return false;
    }
    for (;;) {
        const auto action = shell.PollAction(RemoteAwareTimeout(pdMS_TO_TICKS(250U), command_pump));
        if (command_pump != nullptr && command_pump->Process()) {
            shell.LeaveSystemInformation();
            return true;
        }
        const host_ui::RemoteControlModel latest = remote_control.Snapshot();
        if (FirmwareUpdateInProgress(latest.firmware_update_state)) {
            shell.LeaveSystemInformation();
            return RunFirmwareUpdate(shell, remote_control, command_pump);
        }
        if (!SameFirmwareUpdate(remote_model, latest)) {
            remote_model = latest;
            shell.UpdateSystemInformation(MakeSystemInformationModel(remote_model));
        }
        if (!action.has_value()) {
            continue;
        }
        if (action->type == host_ui::SystemUiActionType::kCloseSystemInformation ||
            action->type == host_ui::SystemUiActionType::kSuspendToHall) {
            shell.LeaveSystemInformation();
            return true;
        }
        if (action->type == host_ui::SystemUiActionType::kInstallFirmwareUpdate) {
            shell.LeaveSystemInformation();
            return RunFirmwareUpdate(shell, remote_control, command_pump);
        }
        ESP_LOGW(kTag, "ignored action=%u while System Information is visible", static_cast<unsigned>(action->type));
    }
}

bool RunRemoteControlSettings(host_ui::SystemShell& shell, host_ui::SystemSettingsStore& settings_store,
                              remote_control::RemoteControlAgent& remote_control, host_ui::RemoteControlModel& model,
                              RemoteCommandPump* command_pump) {
    const auto show_result = shell.ShowRemoteControl(model);
    if (!show_result) {
        ESP_LOGE(kTag, "failed to show Remote Control: error=%u", static_cast<unsigned>(show_result.error()));
        return false;
    }
    for (;;) {
        const auto action = shell.PollAction(pdMS_TO_TICKS(250U));
        if (command_pump != nullptr && command_pump->Process()) {
            shell.LeaveRemoteControl();
            return true;
        }
        const host_ui::RemoteControlModel latest = remote_control.Snapshot();
        if (!SameRemoteControlModel(model, latest)) {
            model = latest;
            shell.UpdateRemoteControl(model);
        }
        if (!action.has_value()) {
            continue;
        }
        if (action->type == host_ui::SystemUiActionType::kCloseRemoteControl ||
            action->type == host_ui::SystemUiActionType::kSuspendToHall) {
            shell.LeaveRemoteControl();
            return true;
        }
        if (action->type == host_ui::SystemUiActionType::kSetRemoteControlEnabled) {
            const bool enabled = action->value != 0U;
            if (!remote_control.SetEnabled(enabled)) {
                ESP_LOGW(kTag, "Remote Control enabled command queue is full");
            }
            model = remote_control.Snapshot();
            if (settings_store.ready() && !settings_store.SaveRemoteControl(model)) {
                ESP_LOGW(kTag, "Remote Control enabled state could not be persisted");
            }
            shell.UpdateRemoteControl(model);
            continue;
        }
        if (action->type == host_ui::SystemUiActionType::kGenerateRemoteControlPairingCode) {
            ESP_LOGI(kTag, "Remote Control pairing action received from System UI");
            const bool queued = remote_control.RequestPairingCode();
            ESP_LOGI(kTag, "Remote Control pairing command enqueue result: %s", queued ? "queued" : "full");
            if (!queued) {
                ESP_LOGW(kTag, "Remote Control pairing command queue is full");
            }
            model = remote_control.Snapshot();
            shell.UpdateRemoteControl(model);
            continue;
        }
        if (action->type == host_ui::SystemUiActionType::kCancelRemoteControlPairingCode) {
            if (!remote_control.CancelPairingCode()) {
                ESP_LOGW(kTag, "Remote Control pairing cancellation queue is full");
            }
            model = remote_control.Snapshot();
            shell.UpdateRemoteControl(model);
            continue;
        }
        ESP_LOGW(kTag, "ignored action=%u while Remote Control is visible", static_cast<unsigned>(action->type));
    }
}

bool SupportedAutoSleepTimeout(uint32_t minutes) {
    return minutes == 1U || minutes == 5U || minutes == 10U || minutes == 30U;
}

bool RunPowerManagement(host_ui::SystemShell& shell, host_ui::StatusLayerModel& status_model,
                        host_ui::SystemSettingsStore& settings_store, RemoteCommandPump* command_pump) {
    auto model = host_ui::PowerManagementModel{
        .auto_sleep_timeout_minutes = status_model.auto_sleep_timeout_minutes,
    };
    const auto show_result = shell.ShowPowerManagement(model);
    if (!show_result) {
        ESP_LOGE(kTag, "failed to show Power Management: error=%u", static_cast<unsigned>(show_result.error()));
        return false;
    }
    for (;;) {
        const auto action = shell.PollAction(RemoteAwareTimeout(portMAX_DELAY, command_pump));
        if (command_pump != nullptr && command_pump->Process()) {
            shell.LeavePowerManagement();
            return true;
        }
        if (!action.has_value()) {
            continue;
        }
        if (action->type == host_ui::SystemUiActionType::kClosePowerManagement ||
            action->type == host_ui::SystemUiActionType::kSuspendToHall) {
            shell.LeavePowerManagement();
            return true;
        }

        uint8_t next_timeout = status_model.auto_sleep_timeout_minutes;
        if (action->type == host_ui::SystemUiActionType::kSetAutoSleepEnabled) {
            next_timeout = action->value != 0U ? host_ui::kDefaultAutoSleepTimeoutMinutes : 0U;
        } else if (action->type == host_ui::SystemUiActionType::kSetAutoSleepTimeout &&
                   SupportedAutoSleepTimeout(action->value)) {
            next_timeout = static_cast<uint8_t>(action->value);
        } else {
            ESP_LOGW(kTag, "ignored action=%u while Power Management is visible", static_cast<unsigned>(action->type));
            continue;
        }

        if (next_timeout == status_model.auto_sleep_timeout_minutes) {
            continue;
        }
        status_model.auto_sleep_timeout_minutes = next_timeout;
        model.auto_sleep_timeout_minutes = next_timeout;
        shell.SetAutoSleepTimeout(next_timeout);
        shell.UpdatePowerManagement(model);
        if (settings_store.ready() && !settings_store.Save(status_model)) {
            ESP_LOGW(kTag, "Auto sleep setting changed but could not be persisted");
        }
    }
}

bool RunAppManagement(host_ui::SystemShell& shell, const runtime::InstalledAppCatalog& catalog, bool launch_available,
                      const AppManagementUninstallHandler* uninstall_handler, std::optional<uint32_t>& launch_request,
                      RemoteCommandPump* command_pump) {
    const bool uninstall_available = uninstall_handler != nullptr && uninstall_handler->available;
    auto show_result = shell.ShowAppManagement(MakeAppManagementModel(catalog, launch_available, uninstall_available));
    if (!show_result) {
        ESP_LOGE(kTag, "failed to show App Management: error=%u", static_cast<unsigned>(show_result.error()));
        return false;
    }
    for (;;) {
        const auto action = shell.PollAction(RemoteAwareTimeout(portMAX_DELAY, command_pump));
        if (command_pump != nullptr && command_pump->Process()) {
            shell.LeaveAppManagement();
            return true;
        }
        if (!action.has_value()) {
            continue;
        }
        if (action->type == host_ui::SystemUiActionType::kCloseAppManagement ||
            action->type == host_ui::SystemUiActionType::kSuspendToHall) {
            shell.LeaveAppManagement();
            return true;
        }
        if (action->type == host_ui::SystemUiActionType::kLaunchInstalledApp && launch_available &&
            action->app_index < catalog.count) {
            launch_request = action->app_index;
            shell.LeaveAppManagement();
            return true;
        }
        if (action->type == host_ui::SystemUiActionType::kUninstallInstalledApp) {
            if (!uninstall_available || action->app_index >= catalog.count) {
                ESP_LOGW(kTag, "ignored unavailable local uninstall request: index=%" PRIu32, action->app_index);
                continue;
            }
            shell.LeaveAppManagement();
            if (!uninstall_handler->Uninstall(action->app_index)) {
                ESP_LOGE(kTag, "local App uninstall failed: index=%" PRIu32, action->app_index);
            }
            show_result =
                shell.ShowAppManagement(MakeAppManagementModel(catalog, launch_available, uninstall_available));
            if (!show_result) {
                ESP_LOGE(kTag, "failed to refresh App Management after uninstall: error=%u",
                         static_cast<unsigned>(show_result.error()));
                return false;
            }
            continue;
        }
        ESP_LOGW(kTag, "ignored action=%u while App Management is visible", static_cast<unsigned>(action->type));
    }
}

bool RunSystemMenu(host_ui::SystemShell& shell, device::BatteryBackend& battery, device::WifiBackend& wifi,
                   host_ui::StatusLayerModel& status_model, const runtime::InstalledAppCatalog& catalog,
                   host_ui::SystemSettingsStore& settings_store, remote_control::RemoteControlAgent& remote_control,
                   bool launch_available, const AppManagementUninstallHandler* uninstall_handler,
                   std::optional<uint32_t>& launch_request, RemoteCommandPump* command_pump) {
    RefreshWifiStatus(status_model, wifi.Snapshot());
    host_ui::RemoteControlModel remote_control_model = remote_control.Snapshot();
    auto show_result = shell.ShowSystemMenu(MakeSystemMenuModel(status_model, catalog, remote_control_model));
    if (!show_result) {
        ESP_LOGE(kTag, "failed to show System Settings: error=%u", static_cast<unsigned>(show_result.error()));
        return false;
    }

    CpuUsageSampler cpu_sampler;
    cpu_sampler.Reset();
    (void)cpu_sampler.Sample();
    int64_t next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
    for (;;) {
        const TickType_t timeout = status_model.performance_overlay_enabled
                                       ? DeadlineWaitTimeout(next_performance_sample_us)
                                       : pdMS_TO_TICKS(250U);
        const auto action = shell.PollAction(RemoteAwareTimeout(timeout, command_pump));
        if (command_pump != nullptr && command_pump->Process()) {
            shell.LeaveSystemMenu();
            return true;
        }
        const int64_t now_us = esp_timer_get_time();
        if (status_model.performance_overlay_enabled && now_us >= next_performance_sample_us) {
            shell.UpdatePerformanceOverlay(true, cpu_sampler.Sample());
            next_performance_sample_us = now_us + kPerformanceSamplePeriodUs;
        }
        const host_ui::RemoteControlModel latest_remote_control = remote_control.Snapshot();
        if (!SameRemoteControlModel(remote_control_model, latest_remote_control)) {
            remote_control_model = latest_remote_control;
            shell.UpdateSystemMenu(MakeSystemMenuModel(status_model, catalog, remote_control_model));
        }
        if (!action.has_value()) {
            continue;
        }
        switch (action->type) {
            case host_ui::SystemUiActionType::kCloseSystemMenu:
            case host_ui::SystemUiActionType::kSuspendToHall:
                shell.LeaveSystemMenu();
                return true;
            case host_ui::SystemUiActionType::kWifiStateChanged:
                RefreshWifiStatus(status_model, wifi.Snapshot());
                shell.UpdateSystemMenu(MakeSystemMenuModel(status_model, catalog, remote_control_model));
                break;
            case host_ui::SystemUiActionType::kSelectSystemMenuItem:
                if (action->value == static_cast<uint32_t>(host_ui::SystemMenuItem::kWifi)) {
                    shell.LeaveSystemMenu();
                    if (!RunWifiSettings(shell, wifi, status_model, command_pump)) {
                        return false;
                    }
                    if (command_pump != nullptr && command_pump->unwind_requested) {
                        return true;
                    }
                    RefreshWifiStatus(status_model, wifi.Snapshot());
                    show_result =
                        shell.ShowSystemMenu(MakeSystemMenuModel(status_model, catalog, remote_control_model));
                    if (!show_result) {
                        ESP_LOGE(kTag, "failed to restore System Settings after Wi-Fi: error=%u",
                                 static_cast<unsigned>(show_result.error()));
                        return false;
                    }
                } else if (action->value == static_cast<uint32_t>(host_ui::SystemMenuItem::kRemoteControl)) {
                    shell.LeaveSystemMenu();
                    if (!RunRemoteControlSettings(shell, settings_store, remote_control, remote_control_model,
                                                  command_pump)) {
                        return false;
                    }
                    if (command_pump != nullptr && command_pump->unwind_requested) {
                        return true;
                    }
                    RefreshWifiStatus(status_model, wifi.Snapshot());
                    remote_control_model = remote_control.Snapshot();
                    show_result =
                        shell.ShowSystemMenu(MakeSystemMenuModel(status_model, catalog, remote_control_model));
                    if (!show_result) {
                        ESP_LOGE(kTag, "failed to restore System Settings after Remote Control: error=%u",
                                 static_cast<unsigned>(show_result.error()));
                        return false;
                    }
                } else if (action->value == static_cast<uint32_t>(host_ui::SystemMenuItem::kPowerManagement)) {
                    shell.LeaveSystemMenu();
                    if (!RunPowerManagement(shell, status_model, settings_store, command_pump)) {
                        return false;
                    }
                    if (command_pump != nullptr && command_pump->unwind_requested) {
                        return true;
                    }
                    show_result =
                        shell.ShowSystemMenu(MakeSystemMenuModel(status_model, catalog, remote_control_model));
                    if (!show_result) {
                        ESP_LOGE(kTag, "failed to restore System Settings after Power Management: error=%u",
                                 static_cast<unsigned>(show_result.error()));
                        return false;
                    }
                } else if (action->value == static_cast<uint32_t>(host_ui::SystemMenuItem::kSystemInformation)) {
                    shell.LeaveSystemMenu();
                    if (!RunSystemInformation(shell, remote_control, command_pump)) {
                        return false;
                    }
                    if (command_pump != nullptr && command_pump->unwind_requested) {
                        return true;
                    }
                    RefreshWifiStatus(status_model, wifi.Snapshot());
                    show_result =
                        shell.ShowSystemMenu(MakeSystemMenuModel(status_model, catalog, remote_control_model));
                    if (!show_result) {
                        ESP_LOGE(kTag, "failed to restore System Settings after System Information: error=%u",
                                 static_cast<unsigned>(show_result.error()));
                        return false;
                    }
                } else if (action->value == static_cast<uint32_t>(host_ui::SystemMenuItem::kManageApps)) {
                    shell.LeaveSystemMenu();
                    if (!RunAppManagement(shell, catalog, launch_available, uninstall_handler, launch_request,
                                          command_pump)) {
                        return false;
                    }
                    if (command_pump != nullptr && command_pump->unwind_requested) {
                        return true;
                    }
                    if (launch_request.has_value()) {
                        return true;
                    }
                    RefreshWifiStatus(status_model, wifi.Snapshot());
                    show_result =
                        shell.ShowSystemMenu(MakeSystemMenuModel(status_model, catalog, remote_control_model));
                    if (!show_result) {
                        ESP_LOGE(kTag, "failed to restore System Settings after App Management: error=%u",
                                 static_cast<unsigned>(show_result.error()));
                        return false;
                    }
                } else {
                    ESP_LOGI(kTag, "System Settings selected item=%" PRIu32 " (service not connected yet)",
                             action->value);
                }
                break;
            case host_ui::SystemUiActionType::kOpenStatusLayer:
                shell.LeaveSystemMenu();
                if (!RunStatusLayer(shell, nullptr, battery, wifi, status_model, catalog, settings_store, command_pump,
                                    action->timestamp_us)) {
                    return false;
                }
                if (command_pump != nullptr && command_pump->unwind_requested) {
                    return true;
                }
                RefreshWifiStatus(status_model, wifi.Snapshot());
                show_result = shell.ShowSystemMenu(MakeSystemMenuModel(status_model, catalog, remote_control_model));
                if (!show_result) {
                    ESP_LOGE(kTag, "failed to restore System Settings after status layer: error=%u",
                             static_cast<unsigned>(show_result.error()));
                    return false;
                }
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                break;
            default:
                ESP_LOGW(kTag, "ignored action=%u while System Settings is visible",
                         static_cast<unsigned>(action->type));
                break;
        }
    }
}

void RunUnavailableHall(host_ui::SystemShell& shell, device::BatteryBackend& battery, device::WifiBackend& wifi,
                        device::PowerBackend& power, HostPowerStateMachine& power_state,
                        const runtime::InstalledAppCatalog& catalog, host_ui::HallStatus status, uint32_t detail,
                        host_ui::StatusLayerModel& status_model, host_ui::SystemSettingsStore& settings_store,
                        remote_control::RemoteControlAgent& remote_control) {
    HallCoverMappings covers = OpenHallCovers(catalog);
    RemoteCommandPump power_pump{
        .poll = [](void* context) { return static_cast<host_ui::SystemShell*>(context)->PowerTransitionRequested(); },
        .context = &shell,
    };
    CpuUsageSampler cpu_sampler;
    cpu_sampler.Reset();
    (void)cpu_sampler.Sample();
    int64_t next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
    for (;;) {
        if (shell.PowerOffRequested()) {
            if (FirmwareUpdateInProgress(remote_control.Snapshot().firmware_update_state)) {
                (void)shell.ConsumePowerOffRequested();
                ESP_LOGW(kTag, "power off ignored while firmware update is in progress");
            } else {
                RunBasicShutdown(shell, power, power_state, remote_control);
            }
        }
        if (shell.PowerButtonPressed()) {
            if (FirmwareUpdateInProgress(remote_control.Snapshot().firmware_update_state)) {
                (void)shell.ConsumePowerButtonPressed();
                shell.NotifyPowerCycleCompleted();
                ESP_LOGW(kTag, "power button ignored while firmware update is in progress");
            } else {
                RunBasicPowerCycle(shell, status_model, power, power_state);
            }
            power_pump.unwind_requested = false;
        }
        if (FirmwareUpdateInProgress(remote_control.Snapshot().firmware_update_state)) {
            (void)RunFirmwareUpdate(shell, remote_control, &power_pump);
            power_pump.unwind_requested = false;
        }
        const device::WifiSnapshot wifi_snapshot = wifi.Snapshot();
        RefreshWifiStatus(status_model, wifi_snapshot);
        const device::BatterySnapshot battery_snapshot = battery.Snapshot();
        const host_ui::RemoteControlModel remote_control_snapshot = remote_control.Snapshot();
        const bool firmware_update_available = remote_control_snapshot.firmware_update_available;
        if (!ShowHall(shell, MakeHallModel(catalog, wifi_snapshot, battery_snapshot, status, nullptr, detail, false,
                                           &covers, std::nullopt, nullptr, 0U, firmware_update_available))) {
            return;
        }
        shell.UpdatePerformanceOverlay(status_model.performance_overlay_enabled, 0U);
        int64_t next_hall_status_sample_us = esp_timer_get_time() + kHallStatusSamplePeriodUs;
        for (;;) {
            const TickType_t timeout = DeadlineWaitTimeout(
                status_model.performance_overlay_enabled ? next_performance_sample_us : next_hall_status_sample_us);
            const auto action = shell.PollAction(timeout);
            if (shell.PowerOffRequested()) {
                break;
            }
            if (shell.PowerButtonPressed()) {
                RunBasicPowerCycle(shell, status_model, power, power_state);
                power_pump.unwind_requested = false;
                break;
            }
            const int64_t now_us = esp_timer_get_time();
            if (status_model.performance_overlay_enabled && now_us >= next_performance_sample_us) {
                shell.UpdatePerformanceOverlay(true, cpu_sampler.Sample());
                next_performance_sample_us = now_us + kPerformanceSamplePeriodUs;
            }
            if (now_us >= next_hall_status_sample_us) {
                shell.UpdateHallStatusBar(MakeHallStatusBarModel(wifi.Snapshot(), battery.Snapshot()));
                next_hall_status_sample_us = now_us + kHallStatusSamplePeriodUs;
            }
            const host_ui::RemoteControlModel latest_remote_control = remote_control.Snapshot();
            if (FirmwareUpdateInProgress(latest_remote_control.firmware_update_state) ||
                latest_remote_control.firmware_update_available != firmware_update_available) {
                break;
            }
            if (!action.has_value()) {
                continue;
            }
            if (action->type == host_ui::SystemUiActionType::kWifiStateChanged) {
                const device::WifiSnapshot refreshed_wifi = wifi.Snapshot();
                RefreshWifiStatus(status_model, refreshed_wifi);
                shell.UpdateHallStatusBar(MakeHallStatusBarModel(refreshed_wifi, battery.Snapshot()));
                continue;
            }
            if (action->type == host_ui::SystemUiActionType::kBatteryStateChanged) {
                shell.UpdateHallStatusBar(MakeHallStatusBarModel(wifi.Snapshot(), battery.Snapshot()));
                next_hall_status_sample_us = esp_timer_get_time() + kHallStatusSamplePeriodUs;
                continue;
            }
            if (action->type == host_ui::SystemUiActionType::kOpenWifiSettings) {
                (void)RunWifiSettings(shell, wifi, status_model, &power_pump);
                if (power_pump.unwind_requested) {
                    power_pump.unwind_requested = false;
                    break;
                }
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                break;
            }
            if (action->type == host_ui::SystemUiActionType::kOpenSystemMenu) {
                std::optional<uint32_t> launch_request;
                (void)RunSystemMenu(shell, battery, wifi, status_model, catalog, settings_store, remote_control, false,
                                    nullptr, launch_request, &power_pump);
                if (power_pump.unwind_requested) {
                    power_pump.unwind_requested = false;
                    break;
                }
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                break;
            }
            if (action->type == host_ui::SystemUiActionType::kOpenFirmwareUpdate) {
                (void)RunFirmwareUpdate(shell, remote_control, &power_pump);
                if (power_pump.unwind_requested) {
                    power_pump.unwind_requested = false;
                    break;
                }
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                break;
            }
            if (action->type != host_ui::SystemUiActionType::kOpenStatusLayer) {
                ESP_LOGW(kTag, "ignored action=%u while App launch is unavailable",
                         static_cast<unsigned>(action->type));
                continue;
            }
            (void)RunStatusLayer(shell, nullptr, battery, wifi, status_model, catalog, settings_store, &power_pump,
                                 action->timestamp_us);
            if (power_pump.unwind_requested) {
                power_pump.unwind_requested = false;
                break;
            }
            cpu_sampler.Reset();
            (void)cpu_sampler.Sample();
            next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
            break;
        }
    }
}

class ActiveHost final {
   public:
    ActiveHost(runtime::InstalledAppCatalog&& catalog, runtime::AppRuntime& runtime, device::DeviceServices& devices,
               host_ui::SystemShell& shell, device::BatteryBackend& battery, device::WifiBackend& wifi,
               device::PowerBackend& power, HostPowerStateMachine& power_state, host_ui::StatusLayerModel& status_model,
               host_ui::SystemSettingsStore& settings_store, remote_control::RemoteControlAgent& remote_control)
        : catalog_(std::move(catalog)),
          covers_(OpenHallCovers(catalog_)),
          app_controller_(runtime),
          devices_(devices),
          shell_(shell),
          battery_(battery),
          wifi_(wifi),
          power_(power),
          power_state_(power_state),
          status_model_(status_model),
          settings_store_(settings_store),
          remote_control_(remote_control),
          hall_status_(catalog_.count == 0U ? host_ui::HallStatus::kNoApps : host_ui::HallStatus::kReady) {
        UpdateRemoteControlCatalog(remote_control_, catalog_);
        remote_control_.UpdateAppLifecycle(nullptr, "not_running");
    }

    [[nodiscard]] bool Valid() const { return app_controller_.valid(); }

    [[nodiscard]] const runtime::InstalledAppCatalog& catalog() const { return catalog_; }

    void Run() {
        for (;;) {
            if (shell_.PowerOffRequested()) {
                if (FirmwareUpdateInProgress(remote_control_.Snapshot().firmware_update_state)) {
                    (void)shell_.ConsumePowerOffRequested();
                    ESP_LOGW(kTag, "power off ignored while firmware update is in progress");
                } else {
                    RunShutdown();
                }
                continue;
            }
            if (shell_.PowerButtonPressed()) {
                RunPowerCycle();
                continue;
            }
            switch (state_) {
                case State::kHall:
                    if (!RunHall()) {
                        return;
                    }
                    break;
                case State::kForeground:
                    RunForeground();
                    break;
            }
        }
    }

   private:
    enum class State { kHall, kForeground };

    [[nodiscard]] bool CanLaunch() const {
        const AppLifecycleState lifecycle = app_controller_.state();
        return catalog_.count != 0U &&
               (lifecycle == AppLifecycleState::kNotRunning || lifecycle == AppLifecycleState::kSuspended);
    }

    [[nodiscard]] bool ShowCurrentHall() {
        const device::WifiSnapshot wifi_snapshot = wifi_.Snapshot();
        RefreshWifiStatus(status_model_, wifi_snapshot);
        const host_ui::RemoteControlModel remote_control_snapshot = remote_control_.Snapshot();
        hall_firmware_update_available_ = remote_control_snapshot.firmware_update_available;
        if (!ShowHall(shell_, MakeHallModel(catalog_, wifi_snapshot, battery_.Snapshot(), hall_status_, outcome_,
                                            hall_detail_, CanLaunch(), &covers_, suspended_index_, &suspended_snapshot_,
                                            hall_transition_trigger_us_, hall_firmware_update_available_))) {
            return false;
        }
        hall_transition_trigger_us_ = 0U;
        shell_.UpdatePerformanceOverlay(status_model_.performance_overlay_enabled, 0U);
        if (!ready_logged_) {
            ESP_LOGI(kTag, "System Shell ready: App Hall rendered with apps=%" PRIu32, catalog_.count);
            ready_logged_ = true;
        }
        return true;
    }

    [[nodiscard]] bool CaptureRemoteScreen(const char* capture_id, remote_control::RemoteControlHostResult& result) {
        if (result.artifact_count >= result.artifacts.size()) {
            return false;
        }
        auto capture_result = shell_.CaptureScreenJpeg();
        if (!capture_result || !capture_result->valid()) {
            ESP_LOGW(kTag, "Remote Control screen capture failed: error=%u",
                     capture_result ? 0U : static_cast<unsigned>(capture_result.error()));
            return false;
        }
        host_ui::ScreenCapture capture = std::move(*capture_result);
        auto& artifact = result.artifacts[result.artifact_count++];
        std::snprintf(artifact.capture_id.data(), artifact.capture_id.size(), "%s",
                      capture_id != nullptr ? capture_id : "screen");
        artifact.size = capture.size();
        artifact.width = capture.width();
        artifact.height = capture.height();
        artifact.release = capture.releaser();
        artifact.data = capture.Detach();
        ESP_LOGI(kTag, "Remote Control screen captured: bytes=%zu dimensions=%" PRIu32 "x%" PRIu32, artifact.size,
                 artifact.width, artifact.height);
        return artifact.data != nullptr;
    }

    [[nodiscard]] std::optional<uint32_t> FindApp(const char* app_id) const {
        if (app_id == nullptr || app_id[0] == '\0') {
            return std::nullopt;
        }
        for (uint32_t index = 0U; index < catalog_.count; ++index) {
            if (std::strcmp(catalog_.apps[index].app_id.data(), app_id) == 0) {
                return index;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool ReloadAppCatalog() {
        // Preserve the active catalog if scanning fails without placing the
        // 3456-byte replacement catalog on the Host supervisor stack.
        auto reloaded_catalog = MakePsramObject<runtime::InstalledAppCatalog>();
        if (reloaded_catalog == nullptr || !runtime::ScanInstalledApps(*reloaded_catalog)) {
            return false;
        }
        catalog_ = std::move(*reloaded_catalog);
        covers_ = OpenHallCovers(catalog_);
        UpdateRemoteControlCatalog(remote_control_, catalog_);
        RefreshStatusMetrics(status_model_, catalog_, battery_);
        hall_status_ = catalog_.count == 0U ? host_ui::HallStatus::kNoApps : host_ui::HallStatus::kReady;
        hall_detail_ = 0U;
        outcome_ = nullptr;
        return true;
    }

    void ReleaseHallCovers() {
        shell_.PauseHallCoverLoading();
        covers_ = {};
    }

    void RestoreHallCovers() { covers_ = OpenHallCovers(catalog_, suspended_index_); }

    [[nodiscard]] bool UninstallInstalledApp(uint32_t app_index) {
        if (app_controller_.state() != AppLifecycleState::kNotRunning || app_index >= catalog_.count) {
            return false;
        }
        const auto app_id = catalog_.apps[app_index].app_id;
        ESP_LOGI(kTag, "uninstalling App from System Settings: index=%" PRIu32 " app=%s", app_index, app_id.data());
        ReleaseHallCovers();
        const auto uninstall_result = runtime::UninstallApp(app_id.data());
        if (!uninstall_result) {
            ESP_LOGE(kTag, "System Settings App uninstall failed: app=%s error=%s", app_id.data(),
                     AppStoreErrorText(uninstall_result.error()));
            RestoreHallCovers();
            return false;
        }
        if (!ReloadAppCatalog()) {
            ESP_LOGE(kTag, "App catalog refresh failed after uninstalling app=%s", app_id.data());
            RestoreHallCovers();
            return false;
        }
        ESP_LOGI(kTag, "System Settings App uninstall completed: app=%s", app_id.data());
        return true;
    }

    void SubmitRemoteResult(remote_control::RemoteControlHostResult& result, bool ok, const char* message) {
        result.ok = ok;
        std::snprintf(result.message.data(), result.message.size(), "%s", message != nullptr ? message : "");
        if (!remote_control_.SubmitHostResult(result)) {
            ESP_LOGW(kTag, "Remote Control result queue is full: command=%.16s", result.command_id.data());
        }
    }

    void FinishPendingStart(bool ok, const char* message, const runtime::AppRunOutcome* outcome = nullptr) {
        if (!pending_start_active_) {
            return;
        }
        if (outcome != nullptr && outcome->completion == runtime::AppCompletion::kFailed) {
            AddAppDiagnostic(pending_start_result_, *outcome);
        }
        SubmitRemoteResult(pending_start_result_, ok, message);
        pending_start_result_ = {};
        pending_start_deadline_ticks_ = 0U;
        pending_start_active_ = false;
    }

    void ReportAppFailure(const runtime::AppRunOutcome& outcome) {
        remote_control::RemoteControlHostResult event{};
        AddAppDiagnostic(event, outcome);
        if (!remote_control_.SubmitHostResult(event)) {
            ESP_LOGW(kTag, "Remote Control App failure queue is full: app=%s", outcome.app_id.data());
        }
    }

    struct RemoteInputSequenceState final {
        remote_control::RemoteControlHostCommand command{};
        remote_control::RemoteControlHostResult result{};
        std::array<device::TouchSample, remote_control::kRemoteControlMaxSequenceOperations> active_touches{};
        std::array<device::KeySample, remote_control::kRemoteControlMaxSequenceOperations> active_keys{};
        size_t active_touch_count{};
        size_t active_key_count{};
        uint32_t next_operation{};
        TickType_t operation_ready_ticks{};
        bool delay_armed{};
        bool active{};
    };

    [[nodiscard]] static RemoteInputSequenceState& RemoteInputSequence() {
        // There is exactly one Host supervisor. Keep this bounded protocol
        // workspace out of app_main's stack while a sequence spans UI/App
        // iterations and possibly crosses System UI screens.
        static RemoteInputSequenceState state;
        return state;
    }

    static bool TickReached(TickType_t now, TickType_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }

    void FinishRemoteInputSequence(bool ok, const char* message) {
        auto& state = RemoteInputSequence();
        bool cleanup_ok = true;
        for (size_t index = 0U; index < state.active_touch_count; ++index) {
            device::TouchSample cancel = state.active_touches[index];
            cancel.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
            cancel.pressure_per_mille = 0U;
            cancel.phase = device::TouchPhase::kCancel;
            cleanup_ok = devices_.input().InjectTouch(cancel) && cleanup_ok;
        }
        for (size_t index = 0U; index < state.active_key_count; ++index) {
            device::KeySample cancel = state.active_keys[index];
            cancel.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
            cancel.phase = device::KeyPhase::kCancel;
            cancel.repeat_count = 0U;
            cleanup_ok = devices_.input().InjectKey(cancel) && cleanup_ok;
        }
        SubmitRemoteResult(state.result, ok && cleanup_ok, ok && !cleanup_ok ? "sequence_failed" : message);
        state.active = false;
    }

    void BeginRemoteInputSequence(const remote_control::RemoteControlHostCommand& command) {
        auto& state = RemoteInputSequence();
        state = {};
        state.command = command;
        state.result.command_id = command.command_id;
        state.active = true;
    }

    void ContinueRemoteInputSequence() {
        auto& state = RemoteInputSequence();
        if (!state.active) {
            return;
        }
        const TickType_t now = xTaskGetTickCount();
        if (state.command.deadline_ticks != 0U && TickReached(now, state.command.deadline_ticks)) {
            FinishRemoteInputSequence(false, "command_expired");
            return;
        }
        if (state.next_operation >= state.command.operation_count) {
            FinishRemoteInputSequence(true, "sequence_completed");
            return;
        }

        const auto& operation = state.command.operations[state.next_operation];
        if (!state.delay_armed) {
            state.operation_ready_ticks = now + pdMS_TO_TICKS(operation.delay_ms);
            state.delay_armed = true;
        }
        if (!TickReached(now, state.operation_ready_ticks)) {
            return;
        }
        state.delay_armed = false;

        if (operation.type == remote_control::RemoteControlSequenceOperationType::kTouch) {
            device::TouchSample sample = operation.touch;
            sample.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
            if (!devices_.input().InjectTouch(sample)) {
                FinishRemoteInputSequence(false, "sequence_failed");
                return;
            }
            auto active =
                std::find_if(state.active_touches.begin(), state.active_touches.begin() + state.active_touch_count,
                             [&](const device::TouchSample& candidate) { return candidate.id == sample.id; });
            if (sample.phase == device::TouchPhase::kDown || sample.phase == device::TouchPhase::kMove) {
                if (active != state.active_touches.begin() + state.active_touch_count) {
                    *active = sample;
                } else if (sample.phase == device::TouchPhase::kDown &&
                           state.active_touch_count < state.active_touches.size()) {
                    state.active_touches[state.active_touch_count++] = sample;
                }
            } else if (active != state.active_touches.begin() + state.active_touch_count) {
                std::move(active + 1, state.active_touches.begin() + state.active_touch_count, active);
                state.active_touches[--state.active_touch_count] = {};
            }
        } else if (operation.type == remote_control::RemoteControlSequenceOperationType::kKey) {
            device::KeySample sample = operation.key;
            sample.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
            if (!devices_.input().InjectKey(sample)) {
                FinishRemoteInputSequence(false, "sequence_failed");
                return;
            }
            auto active =
                std::find_if(state.active_keys.begin(), state.active_keys.begin() + state.active_key_count,
                             [&](const device::KeySample& candidate) { return candidate.code == sample.code; });
            if (sample.phase == device::KeyPhase::kDown || sample.phase == device::KeyPhase::kRepeat) {
                if (active != state.active_keys.begin() + state.active_key_count) {
                    *active = sample;
                } else if (sample.phase == device::KeyPhase::kDown &&
                           state.active_key_count < state.active_keys.size()) {
                    state.active_keys[state.active_key_count++] = sample;
                }
            } else if (active != state.active_keys.begin() + state.active_key_count) {
                std::move(active + 1, state.active_keys.begin() + state.active_key_count, active);
                state.active_keys[--state.active_key_count] = {};
            }
        } else if (!CaptureRemoteScreen(operation.capture_id.data(), state.result)) {
            FinishRemoteInputSequence(false, "sequence_failed");
            return;
        }
        ++state.next_operation;
    }

    // Returns true when the command changed the outer Hall/Foreground state
    // and the current loop must yield to the state machine.
    [[nodiscard]] bool ProcessRemoteCommand(const remote_control::RemoteControlHostCommand& command) {
        // Remote commands are consumed exclusively by the Host supervisor
        // task. Reuse bounded workspaces instead of placing the protocol's
        // largest fixed-capacity objects on app_main's Host stack.
        static remote_control::RemoteControlHostResult result;
        result = {};
        result.command_id = command.command_id;
        const auto deadline_reached = [&]() {
            return command.deadline_ticks != 0U &&
                   static_cast<int32_t>(xTaskGetTickCount() - command.deadline_ticks) >= 0;
        };
        if (deadline_reached()) {
            if (command.type == remote_control::RemoteControlHostCommandType::kInstallApp) {
                heap_caps_free(command.package_data);
            }
            SubmitRemoteResult(result, false, "command_expired");
            return false;
        }
        switch (command.type) {
            case remote_control::RemoteControlHostCommandType::kCaptureScreen:
                if (CaptureRemoteScreen("screen", result)) {
                    SubmitRemoteResult(result, true, "screen_captured");
                } else {
                    SubmitRemoteResult(result, false, "screen_capture_failed");
                }
                return false;
            case remote_control::RemoteControlHostCommandType::kInputSequence:
                BeginRemoteInputSequence(command);
                ContinueRemoteInputSequence();
                return false;
            case remote_control::RemoteControlHostCommandType::kStartApp: {
                const std::optional<uint32_t> app_index = FindApp(command.app_id.data());
                if (!app_index.has_value()) {
                    SubmitRemoteResult(result, false, "app_not_found");
                    return false;
                }
                if (app_controller_.state() == AppLifecycleState::kForeground) {
                    const bool already_running = foreground_index_ == *app_index;
                    SubmitRemoteResult(result, already_running, already_running ? "already_running" : "app_active");
                    return false;
                }
                if (!CanLaunch()) {
                    SubmitRemoteResult(result, false, "lifecycle_busy");
                    return false;
                }
                const char* activation_error = ActivateSelectedApp(*app_index);
                if (activation_error != nullptr) {
                    SubmitRemoteResult(result, false, activation_error);
                    return false;
                }
                if (app_controller_.state() == AppLifecycleState::kForeground) {
                    SubmitRemoteResult(result, true, "app_started");
                    return true;
                }
                if (app_controller_.state() != AppLifecycleState::kStarting || pending_start_active_) {
                    SubmitRemoteResult(result, false, "app_start_failed");
                    return false;
                }
                pending_start_result_ = result;
                pending_start_deadline_ticks_ = command.deadline_ticks;
                pending_start_active_ = true;
                return true;
            }
            case remote_control::RemoteControlHostCommandType::kStopApp: {
                const AppLifecycleState lifecycle = app_controller_.state();
                if (lifecycle == AppLifecycleState::kNotRunning) {
                    SubmitRemoteResult(result, true, "already_stopped");
                    return false;
                }
                const uint32_t active_index = suspended_index_.value_or(foreground_index_);
                if (command.app_id[0] != '\0' &&
                    std::strcmp(command.app_id.data(), catalog_.apps[active_index].app_id.data()) != 0) {
                    SubmitRemoteResult(result, false, "app_not_active");
                    return false;
                }
                remote_control_.UpdateAppLifecycle(catalog_.apps[active_index].app_id.data(), "stopping");
                auto stop_result = StopApp(app_controller_);
                if (!stop_result) {
                    remote_control_.UpdateAppLifecycle(catalog_.apps[active_index].app_id.data(),
                                                       RemoteLifecycleText(app_controller_.state()));
                    SubmitRemoteResult(result, false, "app_stop_failed");
                    return false;
                }
                FinishPendingStart(false, "app_start_cancelled");
                if (suspended_index_.has_value()) {
                    shell_.ReleaseGuestSnapshot();
                    suspended_snapshot_ = {};
                    suspended_index_.reset();
                } else {
                    LeaveForegroundUi();
                }
                outcome_ = nullptr;
                hall_status_ = catalog_.count == 0U ? host_ui::HallStatus::kNoApps : host_ui::HallStatus::kReady;
                hall_detail_ = 0U;
                remote_control_.UpdateAppLifecycle(nullptr, "not_running");
                state_ = State::kHall;
                SubmitRemoteResult(result, true, "app_stopped");
                return true;
            }
            case remote_control::RemoteControlHostCommandType::kInstallApp: {
                if (app_controller_.state() != AppLifecycleState::kNotRunning) {
                    heap_caps_free(command.package_data);
                    SubmitRemoteResult(result, false, "stop_active_app_before_install");
                    return false;
                }
                ReleaseHallCovers();
                const runtime::AppInstallRequest request{
                    .data = command.package_data,
                    .size = command.package_size,
                    .expected_app_id = command.app_id.data(),
                    .expected_sha256 = command.package_sha256,
                };
                auto install_result = runtime::InstallApp(request);
                heap_caps_free(command.package_data);
                if (!install_result) {
                    RestoreHallCovers();
                    SubmitRemoteResult(result, false, AppStoreErrorText(install_result.error()));
                    return false;
                }
                if (!ReloadAppCatalog()) {
                    RestoreHallCovers();
                    SubmitRemoteResult(result, false, "catalog_refresh_failed");
                    return false;
                }
                SubmitRemoteResult(result, true, install_result->changed ? "app_installed" : "already_installed");
                return true;
            }
            case remote_control::RemoteControlHostCommandType::kUninstallApp: {
                if (app_controller_.state() != AppLifecycleState::kNotRunning) {
                    SubmitRemoteResult(result, false, "stop_active_app_before_uninstall");
                    return false;
                }
                if (!FindApp(command.app_id.data()).has_value()) {
                    SubmitRemoteResult(result, true, "already_uninstalled");
                    return false;
                }
                ReleaseHallCovers();
                auto uninstall_result = runtime::UninstallApp(command.app_id.data());
                if (!uninstall_result) {
                    RestoreHallCovers();
                    SubmitRemoteResult(result, false, AppStoreErrorText(uninstall_result.error()));
                    return false;
                }
                if (!ReloadAppCatalog()) {
                    RestoreHallCovers();
                    SubmitRemoteResult(result, false, "catalog_refresh_failed");
                    return false;
                }
                SubmitRemoteResult(result, true, "app_uninstalled");
                return true;
            }
        }
        SubmitRemoteResult(result, false, "unsupported_command");
        return false;
    }

    [[nodiscard]] bool ProcessRemoteCommands() {
        if (RemoteInputSequence().active) {
            ContinueRemoteInputSequence();
            return false;
        }
        static remote_control::RemoteControlHostCommand command;
        while (remote_control_.PollHostCommand(command)) {
            ESP_LOGI(kTag, "Processing Remote Control Host command: type=%u", static_cast<unsigned>(command.type));
            if (ProcessRemoteCommand(command)) {
                return true;
            }
            if (RemoteInputSequence().active) {
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] bool ProcessRemoteCommandsInSystemUi() {
        if (shell_.PowerTransitionRequested()) {
            return true;
        }
        if (FirmwareUpdateInProgress(remote_control_.Snapshot().firmware_update_state)) {
            return true;
        }
        if (RemoteInputSequence().active) {
            ContinueRemoteInputSequence();
            return false;
        }
        static remote_control::RemoteControlHostCommand command;
        while (remote_control_.PeekHostCommand(command)) {
            if (command.type != remote_control::RemoteControlHostCommandType::kCaptureScreen &&
                command.type != remote_control::RemoteControlHostCommandType::kInputSequence) {
                return true;
            }
            if (!remote_control_.PollHostCommand(command)) {
                return false;
            }
            ESP_LOGI(kTag, "Processing Remote Control command in System UI: type=%u",
                     static_cast<unsigned>(command.type));
            if (ProcessRemoteCommand(command)) {
                return true;
            }
            if (RemoteInputSequence().active) {
                return false;
            }
        }
        return false;
    }

    void RecordHostFailure(uint32_t detail) {
        outcome_ = nullptr;
        hall_status_ = host_ui::HallStatus::kHostFailure;
        hall_detail_ = detail;
    }

    [[nodiscard]] bool RunHall() {
        if (shell_.PowerTransitionRequested()) {
            return true;
        }
        if (FirmwareUpdateInProgress(remote_control_.Snapshot().firmware_update_state)) {
            if (!RunFirmwareUpdate(shell_, remote_control_, nullptr)) {
                return false;
            }
        }
        if (shell_.PowerTransitionRequested()) {
            return true;
        }
        if (!ShowCurrentHall()) {
            return false;
        }

        RemoteCommandPump command_pump{
            .poll = [](void* context) { return static_cast<ActiveHost*>(context)->ProcessRemoteCommandsInSystemUi(); },
            .requires_periodic_poll =
                [](void* context) { return static_cast<ActiveHost*>(context)->RemoteInputSequence().active; },
            .context = this,
        };

        CpuUsageSampler cpu_sampler;
        cpu_sampler.Reset();
        (void)cpu_sampler.Sample();
        int64_t next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
        int64_t next_hall_status_sample_us = esp_timer_get_time() + kHallStatusSamplePeriodUs;
        for (;;) {
            const TickType_t timeout = DeadlineWaitTimeout(
                status_model_.performance_overlay_enabled ? next_performance_sample_us : next_hall_status_sample_us);
            const auto pending_action = shell_.PollAction(RemoteAwareTimeout(timeout, &command_pump));
            if (shell_.PowerTransitionRequested()) {
                return true;
            }
            if (ProcessRemoteCommands()) {
                return true;
            }
            const int64_t now_us = esp_timer_get_time();
            if (status_model_.performance_overlay_enabled && now_us >= next_performance_sample_us) {
                shell_.UpdatePerformanceOverlay(true, cpu_sampler.Sample());
                next_performance_sample_us = now_us + kPerformanceSamplePeriodUs;
            }
            if (now_us >= next_hall_status_sample_us) {
                shell_.UpdateHallStatusBar(MakeHallStatusBarModel(wifi_.Snapshot(), battery_.Snapshot()));
                next_hall_status_sample_us = now_us + kHallStatusSamplePeriodUs;
            }
            const host_ui::RemoteControlModel remote_control_snapshot = remote_control_.Snapshot();
            if (FirmwareUpdateInProgress(remote_control_snapshot.firmware_update_state) ||
                remote_control_snapshot.firmware_update_available != hall_firmware_update_available_) {
                return true;
            }
            if (!pending_action.has_value()) {
                continue;
            }

            const host_ui::SystemUiAction action = *pending_action;
            if (action.type == host_ui::SystemUiActionType::kWifiStateChanged) {
                const device::WifiSnapshot wifi_snapshot = wifi_.Snapshot();
                RefreshWifiStatus(status_model_, wifi_snapshot);
                shell_.UpdateHallStatusBar(MakeHallStatusBarModel(wifi_snapshot, battery_.Snapshot()));
                continue;
            }
            if (action.type == host_ui::SystemUiActionType::kBatteryStateChanged) {
                shell_.UpdateHallStatusBar(MakeHallStatusBarModel(wifi_.Snapshot(), battery_.Snapshot()));
                next_hall_status_sample_us = esp_timer_get_time() + kHallStatusSamplePeriodUs;
                continue;
            }
            if (action.type == host_ui::SystemUiActionType::kLaunchApp && action.app_index < catalog_.count &&
                CanLaunch()) {
                ActivateSelectedApp(action.app_index);
                return true;
            }
            if (action.type == host_ui::SystemUiActionType::kStopApp && suspended_index_.has_value() &&
                action.app_index == *suspended_index_) {
                if (!StopSuspendedApp()) {
                    return false;
                }
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                continue;
            }
            if (action.type == host_ui::SystemUiActionType::kOpenStatusLayer) {
                if (!RunStatusLayer(shell_, nullptr, battery_, wifi_, status_model_, catalog_, settings_store_,
                                    &command_pump, action.timestamp_us)) {
                    RecordHostFailure(static_cast<uint32_t>(host_ui::SystemUiError::kRenderFailed));
                }
                if (command_pump.unwind_requested) {
                    return true;
                }
                if (!ShowCurrentHall()) {
                    return false;
                }
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                continue;
            }
            if (action.type == host_ui::SystemUiActionType::kOpenWifiSettings) {
                if (!RunWifiSettings(shell_, wifi_, status_model_, &command_pump)) {
                    RecordHostFailure(static_cast<uint32_t>(host_ui::SystemUiError::kRenderFailed));
                }
                if (command_pump.unwind_requested) {
                    return true;
                }
                if (!ShowCurrentHall()) {
                    return false;
                }
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                continue;
            }
            if (action.type == host_ui::SystemUiActionType::kOpenSystemMenu) {
                std::optional<uint32_t> launch_request;
                const AppManagementUninstallHandler uninstall_handler{
                    .uninstall =
                        [](void* context, uint32_t app_index) {
                            return static_cast<ActiveHost*>(context)->UninstallInstalledApp(app_index);
                        },
                    .context = this,
                    .available = app_controller_.state() == AppLifecycleState::kNotRunning,
                };
                if (!RunSystemMenu(shell_, battery_, wifi_, status_model_, catalog_, settings_store_, remote_control_,
                                   CanLaunch(), &uninstall_handler, launch_request, &command_pump)) {
                    RecordHostFailure(static_cast<uint32_t>(host_ui::SystemUiError::kRenderFailed));
                }
                if (command_pump.unwind_requested) {
                    return true;
                }
                if (!ShowCurrentHall()) {
                    return false;
                }
                if (launch_request.has_value() && *launch_request < catalog_.count && CanLaunch()) {
                    ActivateSelectedApp(*launch_request);
                    return true;
                }
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                continue;
            }
            if (action.type == host_ui::SystemUiActionType::kOpenFirmwareUpdate) {
                if (!RunFirmwareUpdate(shell_, remote_control_, &command_pump)) {
                    RecordHostFailure(static_cast<uint32_t>(host_ui::SystemUiError::kRenderFailed));
                }
                if (command_pump.unwind_requested) {
                    return true;
                }
                if (!ShowCurrentHall()) {
                    return false;
                }
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                continue;
            }
            ESP_LOGW(kTag, "ignored invalid App Hall action: type=%u index=%" PRIu32,
                     static_cast<unsigned>(action.type), action.app_index);
        }
    }

    [[nodiscard]] bool StopSuspendedApp() {
        const uint32_t stopped_index = *suspended_index_;
        ESP_LOGI(kTag, "stopping suspended App from Hall: index=%" PRIu32, stopped_index);
        remote_control_.UpdateAppLifecycle(catalog_.apps[stopped_index].app_id.data(), "stopping");
        auto stopped_result = StopApp(app_controller_);
        if (!stopped_result) {
            ESP_LOGE(kTag, "Hall could not complete suspended App stop: error=%u",
                     static_cast<unsigned>(stopped_result.error()));
            RecordHostFailure(static_cast<uint32_t>(stopped_result.error()));
            remote_control_.UpdateAppLifecycle(catalog_.apps[stopped_index].app_id.data(),
                                               RemoteLifecycleText(app_controller_.state()));
            return ShowCurrentHall();
        }

        remote_control_.UpdateAppLifecycle(nullptr, "not_running");
        suspended_index_.reset();
        outcome_ = nullptr;
        hall_status_ = host_ui::HallStatus::kReady;
        hall_detail_ = 0U;
        if (!ShowCurrentHall()) {
            return false;
        }
        shell_.ReleaseGuestSnapshot();
        suspended_snapshot_ = {};
        micropixel_log_heap_state("host after Hall App stop");
        return true;
    }

    const char* ActivateSelectedApp(uint32_t selected_index) {
        bool resumed_existing = false;
        if (suspended_index_.has_value()) {
            const uint32_t previous_suspended_index = *suspended_index_;
            const bool selected_suspended_app = *suspended_index_ == selected_index;
            bool guest_view_restored = true;
            if (selected_suspended_app) {
                auto restore_result = shell_.RestoreGuestView();
                if (!restore_result) {
                    ESP_LOGE(kTag, "failed to restore retained Guest view: error=%u",
                             static_cast<unsigned>(restore_result.error()));
                    shell_.LeaveHall();
                    guest_view_restored = false;
                }
            } else {
                shell_.LeaveHall();
            }
            shell_.ReleaseGuestSnapshot();
            suspended_snapshot_ = {};
            suspended_index_.reset();

            if (selected_suspended_app) {
                if (!guest_view_restored) {
                    (void)StopApp(app_controller_);
                    RecordHostFailure(static_cast<uint32_t>(host_ui::SystemUiError::kRenderFailed));
                    return "guest_view_restore_failed";
                }
                auto resume_result = app_controller_.Resume();
                if (!resume_result) {
                    ESP_LOGE(kTag, "failed to resume suspended App: error=%u",
                             static_cast<unsigned>(resume_result.error()));
                    auto stopped_result = StopApp(app_controller_);
                    RecordHostFailure(static_cast<uint32_t>(resume_result.error()));
                    if (!stopped_result) {
                        hall_detail_ = static_cast<uint32_t>(stopped_result.error());
                    }
                    remote_control_.UpdateAppLifecycle(nullptr, "not_running");
                    return AppControllerErrorText(resume_result.error());
                }
                remote_control_.UpdateAppLifecycle(catalog_.apps[selected_index].app_id.data(), "foreground");
                ESP_LOGI(kTag, "resumed selected App: index=%" PRIu32, selected_index);
                resumed_existing = true;
            } else {
                ESP_LOGI(kTag, "switching App: stopping suspended index before launching index=%" PRIu32,
                         selected_index);
                auto stopped_result = StopApp(app_controller_);
                if (!stopped_result) {
                    RecordHostFailure(static_cast<uint32_t>(stopped_result.error()));
                    remote_control_.UpdateAppLifecycle(catalog_.apps[previous_suspended_index].app_id.data(),
                                                       RemoteLifecycleText(app_controller_.state()));
                    return AppControllerErrorText(stopped_result.error());
                }
                remote_control_.UpdateAppLifecycle(nullptr, "not_running");
                micropixel_log_heap_state("host after suspended App stop");
            }
        } else {
            shell_.LeaveHall();
        }

        const runtime::InstalledApp& selected_app = catalog_.apps[selected_index];
        if (!resumed_existing) {
            remote_control_.UpdateAppLifecycle(selected_app.app_id.data(), "starting");
            ESP_LOGI(kTag, "launching selected App: index=%" PRIu32 " app=%s bytes=%" PRIu32, selected_index,
                     selected_app.app_id.data(), selected_app.bundle_size);
            micropixel_log_heap_state("host before AppController start");
            auto start_result = app_controller_.Start(selected_app);
            if (!start_result) {
                ESP_LOGE(kTag, "Host could not start the selected AppSession: error=%u",
                         static_cast<unsigned>(start_result.error()));
                RecordHostFailure(static_cast<uint32_t>(start_result.error()));
                remote_control_.UpdateAppLifecycle(nullptr, "not_running");
                return AppControllerErrorText(start_result.error());
            }
        }

        foreground_index_ = selected_index;
        state_ = State::kForeground;
        return nullptr;
    }

    void RunForeground() {
        CpuUsageSampler cpu_sampler;
        cpu_sampler.Reset();
        (void)cpu_sampler.Sample();
        int64_t next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
        shell_.UpdatePerformanceOverlay(status_model_.performance_overlay_enabled, 0U);
        shell_.WatchGuestActions();

        for (;;) {
            if (shell_.PowerTransitionRequested()) {
                return;
            }
            auto completion_result = app_controller_.PollCompletion(pdMS_TO_TICKS(20));
            remote_control_.UpdateAppLifecycle(catalog_.apps[foreground_index_].app_id.data(),
                                               RemoteLifecycleText(app_controller_.state()));
            if (pending_start_active_ && app_controller_.state() == AppLifecycleState::kForeground) {
                FinishPendingStart(true, "app_started");
            }
            if (pending_start_active_ && pending_start_deadline_ticks_ != 0U &&
                static_cast<int32_t>(xTaskGetTickCount() - pending_start_deadline_ticks_) >= 0) {
                FinishPendingStart(false, "command_expired");
                (void)app_controller_.RequestStop();
            }
            if (ProcessRemoteCommands()) {
                return;
            }
            if (FirmwareUpdateInProgress(remote_control_.Snapshot().firmware_update_state)) {
                if (app_controller_.state() == AppLifecycleState::kForeground) {
                    SuspendToHall(0U);
                }
                return;
            }
            if (!completion_result) {
                ESP_LOGE(kTag, "Host could not join the completed AppSession: error=%u",
                         static_cast<unsigned>(completion_result.error()));
                LeaveForegroundUi();
                RecordHostFailure(static_cast<uint32_t>(completion_result.error()));
                FinishPendingStart(false, AppControllerErrorText(completion_result.error()));
                remote_control_.UpdateAppLifecycle(nullptr, "not_running");
                state_ = State::kHall;
                return;
            }
            if (completion_result->has_value()) {
                last_outcome_ = **completion_result;
                if (pending_start_active_) {
                    const bool started_and_exited = last_outcome_.completion == runtime::AppCompletion::kExited;
                    FinishPendingStart(started_and_exited,
                                       started_and_exited ? "app_started_and_exited" : "app_start_failed",
                                       &last_outcome_);
                }
                LeaveForegroundUi();
                outcome_ = &last_outcome_;
                hall_status_ = last_outcome_.completion == runtime::AppCompletion::kFailed
                                   ? host_ui::HallStatus::kAppFailed
                                   : host_ui::HallStatus::kAppExited;
                hall_detail_ = last_outcome_.completion == runtime::AppCompletion::kFailed
                                   ? static_cast<uint32_t>(last_outcome_.error)
                                   : 0U;
                if (last_outcome_.completion == runtime::AppCompletion::kFailed) {
                    ESP_LOGE(kTag, "AppSession failed: app=%s error=%u", last_outcome_.app_id.data(),
                             static_cast<unsigned>(last_outcome_.error));
                    ReportAppFailure(last_outcome_);
                }
                micropixel_log_heap_state("host after AppController completion");
                remote_control_.UpdateAppLifecycle(nullptr, "not_running");
                state_ = State::kHall;
                return;
            }

            const int64_t now_us = esp_timer_get_time();
            if (status_model_.performance_overlay_enabled && now_us >= next_performance_sample_us) {
                shell_.UpdatePerformanceOverlay(true, cpu_sampler.Sample());
                next_performance_sample_us = now_us + kPerformanceSamplePeriodUs;
            }

            const auto action = shell_.PollAction(0U);
            if (shell_.PowerTransitionRequested()) {
                return;
            }
            if (!action.has_value()) {
                continue;
            }
            if (action->type == host_ui::SystemUiActionType::kWifiStateChanged) {
                RefreshWifiStatus(status_model_, wifi_.Snapshot());
                continue;
            }
            if (action->type == host_ui::SystemUiActionType::kOpenStatusLayer) {
                OpenForegroundStatusLayer(action->timestamp_us, cpu_sampler, next_performance_sample_us);
                continue;
            }
            if (action->type == host_ui::SystemUiActionType::kSuspendToHall &&
                app_controller_.state() == AppLifecycleState::kForeground) {
                SuspendToHall(action->timestamp_us);
                return;
            }
            ESP_LOGW(kTag, "ignored Guest system action=%u in lifecycle state=%u", static_cast<unsigned>(action->type),
                     static_cast<unsigned>(app_controller_.state()));
        }
    }

    void OpenForegroundStatusLayer(uint64_t trigger_timestamp_us, CpuUsageSampler& cpu_sampler,
                                   int64_t& next_performance_sample_us) {
        if (app_controller_.state() != AppLifecycleState::kForeground) {
            ESP_LOGW(kTag, "ignored status-layer gesture in lifecycle state=%u",
                     static_cast<unsigned>(app_controller_.state()));
            return;
        }
        remote_control_.UpdateAppLifecycle(catalog_.apps[foreground_index_].app_id.data(), "suspending");
        RemoteCommandPump command_pump{
            .poll = [](void* context) { return static_cast<ActiveHost*>(context)->ProcessRemoteCommandsInSystemUi(); },
            .requires_periodic_poll =
                [](void* context) { return static_cast<ActiveHost*>(context)->RemoteInputSequence().active; },
            .context = this,
        };
        if (!RunStatusLayer(shell_, &app_controller_, battery_, wifi_, status_model_, catalog_, settings_store_,
                            &command_pump, trigger_timestamp_us)) {
            RecordHostFailure(static_cast<uint32_t>(AppControllerError::kResumeFailed));
        }
        remote_control_.UpdateAppLifecycle(catalog_.apps[foreground_index_].app_id.data(),
                                           RemoteLifecycleText(app_controller_.state()));
        cpu_sampler.Reset();
        (void)cpu_sampler.Sample();
        next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
    }

    void RunShutdown() {
        if (!shell_.PowerOffRequested() || !power_state_.BeginShutdown()) {
            return;
        }
        if (!shell_.ConsumePowerOffRequested()) {
            power_state_.RecoverAwake();
            return;
        }

        ESP_LOGI(kTag, "power-off sequence started");
        if (RemoteInputSequence().active) {
            FinishRemoteInputSequence(false, "device_shutting_down");
        }
        FinishPendingStart(false, "device_shutting_down");
        shell_.StopWatchingGuestActions();
        shell_.UpdatePerformanceOverlay(false, 0U);
        shell_.ApplyVolume(0U);

        const AppLifecycleState lifecycle = app_controller_.state();
        if (lifecycle == AppLifecycleState::kNotRunning) {
            const auto completion_result = app_controller_.PollCompletion(pdMS_TO_TICKS(50U));
            if (!completion_result) {
                ESP_LOGW(kTag, "could not collect completed App before power off: error=%u",
                         static_cast<unsigned>(completion_result.error()));
            }
        } else {
            const uint32_t active_index = suspended_index_.value_or(foreground_index_);
            if (active_index < catalog_.count) {
                remote_control_.UpdateAppLifecycle(catalog_.apps[active_index].app_id.data(), "stopping");
            }
            const auto stop_result = StopApp(app_controller_);
            if (!stop_result) {
                ESP_LOGE(kTag, "could not stop App before power off: error=%u",
                         static_cast<unsigned>(stop_result.error()));
                RecordHostFailure(static_cast<uint32_t>(stop_result.error()));
            } else {
                ESP_LOGI(kTag, "App stopped before power off");
            }
        }
        remote_control_.UpdateAppLifecycle(nullptr, "not_running");
        suspended_index_.reset();
        suspended_snapshot_ = {};
        shell_.ReleaseGuestSnapshot();

        const auto show_result = shell_.ShowShutdown();
        if (!show_result) {
            ESP_LOGE(kTag, "could not show shutdown screen: error=%u", static_cast<unsigned>(show_result.error()));
        }
        remote_control_.Stop(kShutdownRemoteStopTimeout);
        ESP_LOGI(kTag, "shutdown cleanup complete; requesting physical power cut");
        power_.PowerOff();
    }

    void RunPowerCycle() {
        if (FirmwareUpdateInProgress(remote_control_.Snapshot().firmware_update_state)) {
            (void)shell_.ConsumePowerButtonPressed();
            shell_.NotifyPowerCycleCompleted();
            ESP_LOGW(kTag, "power button ignored while firmware update is in progress");
            return;
        }
        if (!shell_.PowerButtonPressed() || !power_state_.BeginSleep()) {
            return;
        }
        if (!shell_.ConsumePowerButtonPressed()) {
            power_state_.RecoverAwake();
            return;
        }

        if (RemoteInputSequence().active) {
            FinishRemoteInputSequence(false, "power_transition");
        }

        bool resume_foreground = false;
        bool moved_to_hall = false;
        if (state_ == State::kForeground) {
            const AppLifecycleState lifecycle = app_controller_.state();
            if (lifecycle == AppLifecycleState::kForeground) {
                remote_control_.UpdateAppLifecycle(catalog_.apps[foreground_index_].app_id.data(), "suspending");
                const auto suspend_result = app_controller_.Suspend(kPowerSuspendTimeout);
                if (suspend_result) {
                    resume_foreground = true;
                    shell_.StopWatchingGuestActions();
                    remote_control_.UpdateAppLifecycle(catalog_.apps[foreground_index_].app_id.data(), "suspended");
                } else {
                    ESP_LOGE(kTag, "App did not reach the power suspend safe point: error=%u",
                             static_cast<unsigned>(suspend_result.error()));
                    RecordHostFailure(static_cast<uint32_t>(suspend_result.error()));
                }
            } else if (lifecycle == AppLifecycleState::kSuspended) {
                resume_foreground = true;
            }

            if (!resume_foreground) {
                FinishPendingStart(false, "power_transition");
                if (lifecycle == AppLifecycleState::kNotRunning) {
                    const auto completion_result = app_controller_.PollCompletion(pdMS_TO_TICKS(50U));
                    if (!completion_result) {
                        RecordHostFailure(static_cast<uint32_t>(completion_result.error()));
                    }
                    remote_control_.UpdateAppLifecycle(nullptr, "not_running");
                } else {
                    const auto stopped_result = StopApp(app_controller_);
                    if (!stopped_result) {
                        ESP_LOGE(kTag, "could not stop App before low power: error=%u",
                                 static_cast<unsigned>(stopped_result.error()));
                        RecordHostFailure(static_cast<uint32_t>(stopped_result.error()));
                        remote_control_.UpdateAppLifecycle(catalog_.apps[foreground_index_].app_id.data(),
                                                           RemoteLifecycleText(app_controller_.state()));
                    } else {
                        remote_control_.UpdateAppLifecycle(nullptr, "not_running");
                    }
                }
                suspended_index_.reset();
                suspended_snapshot_ = {};
                shell_.ReleaseGuestSnapshot();
                LeaveForegroundUi();
                state_ = State::kHall;
                moved_to_hall = true;
            }
        }

        FadeBrightness(shell_, status_model_.brightness_percent, 0U);
        if (!power_state_.MarkAsleep()) {
            power_state_.RecoverAwake();
            if (resume_foreground) {
                (void)app_controller_.Resume();
            }
            FadeBrightness(shell_, 0U, status_model_.brightness_percent);
            shell_.NotifyPowerCycleCompleted();
            return;
        }

        const auto sleep_result = power_.EnterLowPower();
        if (!power_state_.BeginWake()) {
            power_state_.RecoverAwake();
        }
        if (!sleep_result) {
            ESP_LOGE(kTag, "low-power cycle failed: %s", PowerErrorText(sleep_result.error()));
        }

        if (resume_foreground) {
            const auto resume_result = app_controller_.Resume();
            if (!resume_result) {
                ESP_LOGE(kTag, "failed to resume App after wake: error=%u",
                         static_cast<unsigned>(resume_result.error()));
                (void)StopApp(app_controller_);
                RecordHostFailure(static_cast<uint32_t>(resume_result.error()));
                remote_control_.UpdateAppLifecycle(nullptr, "not_running");
                LeaveForegroundUi();
                state_ = State::kHall;
                moved_to_hall = true;
            } else {
                remote_control_.UpdateAppLifecycle(catalog_.apps[foreground_index_].app_id.data(), "foreground");
            }
        }

        const bool display_restored =
            sleep_result.has_value() || sleep_result.error() != device::PowerError::kDisplayRestore;
        if (moved_to_hall && display_restored && !ShowCurrentHall()) {
            RecordHostFailure(static_cast<uint32_t>(host_ui::SystemUiError::kRenderFailed));
        }
        FadeBrightness(shell_, 0U, status_model_.brightness_percent);
        if (!power_state_.FinishWake()) {
            power_state_.RecoverAwake();
        }
        shell_.NotifyPowerCycleCompleted();
    }

    void SuspendToHall(uint64_t trigger_timestamp_us) {
        const int64_t suspend_started_us = esp_timer_get_time();
        if (trigger_timestamp_us == 0U) {
            trigger_timestamp_us = static_cast<uint64_t>(suspend_started_us);
        }
        auto suspend_result = app_controller_.Suspend(pdMS_TO_TICKS(500));
        if (!suspend_result) {
            ESP_LOGE(kTag, "failed to suspend App for Hall: error=%u", static_cast<unsigned>(suspend_result.error()));
            RecordHostFailure(static_cast<uint32_t>(suspend_result.error()));
            remote_control_.UpdateAppLifecycle(catalog_.apps[foreground_index_].app_id.data(),
                                               RemoteLifecycleText(app_controller_.state()));
            return;
        }
        remote_control_.UpdateAppLifecycle(catalog_.apps[foreground_index_].app_id.data(), "suspended");
        const int64_t suspend_completed_us = esp_timer_get_time();
        shell_.StopWatchingGuestActions();
        auto snapshot_result = shell_.CaptureGuestFrame(foreground_index_, trigger_timestamp_us);
        const int64_t snapshot_completed_us = esp_timer_get_time();
        if (snapshot_result) {
            suspended_snapshot_ = *snapshot_result;
        } else {
            suspended_snapshot_ = {};
            ESP_LOGW(kTag, "running-card snapshot unavailable: error=%u; using Flash cover",
                     static_cast<unsigned>(snapshot_result.error()));
        }
        suspended_index_ = foreground_index_;
        outcome_ = nullptr;
        hall_status_ = host_ui::HallStatus::kReady;
        hall_detail_ = 0U;
        hall_transition_trigger_us_ = trigger_timestamp_us;
        LeaveForegroundUi();
        ESP_LOGI(kTag,
                 "Hall preparation timing: trigger-to-host=%" PRIu64 " us suspend=%" PRIu64 " us snapshot=%" PRIu64
                 " us trigger-to-snapshot=%" PRIu64 " us",
                 static_cast<uint64_t>(suspend_started_us) - trigger_timestamp_us,
                 static_cast<uint64_t>(suspend_completed_us - suspend_started_us),
                 static_cast<uint64_t>(snapshot_completed_us - suspend_completed_us),
                 static_cast<uint64_t>(snapshot_completed_us) - trigger_timestamp_us);
        ESP_LOGI(kTag, "suspended App moved to Hall: index=%" PRIu32, foreground_index_);
        state_ = State::kHall;
    }

    void LeaveForegroundUi() {
        shell_.StopWatchingGuestActions();
        shell_.UpdatePerformanceOverlay(false, 0U);
    }

    runtime::InstalledAppCatalog catalog_;
    HallCoverMappings covers_;
    AppController app_controller_;
    device::DeviceServices& devices_;
    host_ui::SystemShell& shell_;
    device::BatteryBackend& battery_;
    device::WifiBackend& wifi_;
    device::PowerBackend& power_;
    HostPowerStateMachine& power_state_;
    host_ui::StatusLayerModel& status_model_;
    host_ui::SystemSettingsStore& settings_store_;
    remote_control::RemoteControlAgent& remote_control_;
    State state_{State::kHall};
    runtime::AppRunOutcome last_outcome_{};
    const runtime::AppRunOutcome* outcome_{};
    host_ui::HallStatus hall_status_;
    uint32_t hall_detail_{};
    uint32_t foreground_index_{};
    std::optional<uint32_t> suspended_index_;
    host_ui::HallCoverModel suspended_snapshot_{};
    uint64_t hall_transition_trigger_us_{};
    bool ready_logged_{};
    bool hall_firmware_update_available_{};
    remote_control::RemoteControlHostResult pending_start_result_{};
    TickType_t pending_start_deadline_ticks_{};
    bool pending_start_active_{};
};

}  // namespace

HostController::HostController(device::DeviceServices& devices, device::BatteryBackend& battery,
                               device::WifiBackend& wifi, device::PowerBackend& power, host_ui::SystemShell& shell,
                               remote_control::RemoteControlAgent& remote_control)
    : devices_(devices), battery_(battery), wifi_(wifi), power_(power), shell_(shell), remote_control_(remote_control) {
    battery_.SetStateChangeSink(
        [](void* context) { static_cast<host_ui::SystemShell*>(context)->NotifyBatteryStateChanged(); }, &shell_);
    wifi_.SetStateChangeSink(
        [](void* context) { static_cast<host_ui::SystemShell*>(context)->NotifyWifiStateChanged(); }, &shell_);
    remote_control_.SetHostCommandReadySink(
        [](void* context) { static_cast<host_ui::SystemShell*>(context)->NotifyRemoteCommandReady(); }, &shell_);
    devices_.input().SetActivitySink(
        [](void* context) { static_cast<host_ui::SystemShell*>(context)->NotifyUserActivity(); }, &shell_);
    power_.SetPowerButtonSink(
        [](void* context, uint64_t timestamp_us) {
            auto* controller = static_cast<HostController*>(context);
            if (controller->power_state_.state() != HostPowerState::kAwake) {
                return false;
            }
            return controller->shell_.NotifyPowerButtonPressed(timestamp_us);
        },
        this);
    power_.SetPowerOffButtonSink(
        [](void* context, uint64_t timestamp_us) {
            auto* controller = static_cast<HostController*>(context);
            if (controller->power_state_.state() != HostPowerState::kAwake ||
                controller->shell_.PowerTransitionRequested()) {
                return false;
            }
            return controller->shell_.NotifyPowerOffRequested(timestamp_us);
        },
        this);
}

HostController::~HostController() {
    devices_.input().SetActivitySink(nullptr, nullptr);
    remote_control_.SetHostCommandReadySink(nullptr, nullptr);
    remote_control_.Stop();
    battery_.SetStateChangeSink(nullptr, nullptr);
    wifi_.SetStateChangeSink(nullptr, nullptr);
    power_.SetPowerButtonSink(nullptr, nullptr);
    power_.SetPowerOffButtonSink(nullptr, nullptr);
}

void HostController::Run() {
    vTaskPrioritySet(nullptr, task_policy::kHostPriority);
    ESP_LOGI(kTag, "Host supervisor task priority=%u", static_cast<unsigned>(uxTaskPriorityGet(nullptr)));

    host_ui::SystemSettingsStore settings_store;
    if (!settings_store.Initialize()) {
        ESP_LOGW(kTag, "Host settings persistence is unavailable; using defaults for this boot");
    }
    host_ui::StatusLayerModel status_model{};
    if (settings_store.ready() && !settings_store.Load(status_model)) {
        ESP_LOGW(kTag, "Host settings could not be restored; using safe defaults");
    }
    shell_.ConfigureAutoSleep(
        status_model.auto_sleep_timeout_minutes,
        [](void* context, bool& connected) {
            auto* controller = static_cast<HostController*>(context);
            const device::BatterySnapshot snapshot = controller->battery_.Snapshot();
            connected = snapshot.external_power_connected;
            return snapshot.external_power_available;
        },
        this);
    host_ui::RemoteControlModel remote_control_settings = remote_control_.Snapshot();
    if (settings_store.ready() && !settings_store.LoadRemoteControl(remote_control_settings)) {
        ESP_LOGW(kTag, "Remote Control settings could not be restored; using disabled default");
        remote_control_settings.enabled = false;
    }
    if (!remote_control_.Start(remote_control_settings.enabled)) {
        ESP_LOGW(kTag, "Remote Control agent is unavailable for this boot");
    }
    RefreshWifiStatus(status_model, wifi_.Snapshot());
    shell_.ApplyBrightness(status_model.brightness_percent);
    shell_.ApplyVolume(status_model.volume_percent);

    auto catalog = MakePsramObject<runtime::InstalledAppCatalog>();
    if (catalog == nullptr) {
        ESP_LOGE(kTag, "failed to allocate App Store catalog");
        return;
    }
    auto catalog_result = runtime::ScanInstalledApps(*catalog);
    if (!catalog_result) {
        ESP_LOGE(kTag, "App Store catalog scan failed");
        *catalog = {};
        UpdateRemoteControlCatalog(remote_control_, *catalog);
        remote_control_.UpdateAppLifecycle(nullptr, "not_running");
        RunUnavailableHall(shell_, battery_, wifi_, power_, power_state_, *catalog, host_ui::HallStatus::kNoApps,
                           static_cast<uint32_t>(catalog_result.error()), status_model, settings_store,
                           remote_control_);
        return;
    }
    UpdateRemoteControlCatalog(remote_control_, *catalog);
    remote_control_.UpdateAppLifecycle(nullptr, "not_running");

    auto runtime_result = runtime::AppRuntime::Initialize(devices_, &remote_control_);
    if (!runtime_result) {
        ESP_LOGE(kTag, "AppRuntime initialization failed: error=%u", static_cast<unsigned>(runtime_result.error()));
        RunUnavailableHall(shell_, battery_, wifi_, power_, power_state_, *catalog,
                           host_ui::HallStatus::kRuntimeUnavailable, static_cast<uint32_t>(runtime_result.error()),
                           status_model, settings_store, remote_control_);
        return;
    }

    runtime::AppRuntime app_runtime = std::move(*runtime_result);
    auto active_host = MakePsramObject<ActiveHost>(std::move(*catalog), app_runtime, devices_, shell_, battery_, wifi_,
                                                   power_, power_state_, status_model, settings_store, remote_control_);
    if (active_host == nullptr) {
        ESP_LOGE(kTag, "failed to allocate ActiveHost state");
        RunUnavailableHall(
            shell_, battery_, wifi_, power_, power_state_, *catalog, host_ui::HallStatus::kRuntimeUnavailable,
            static_cast<uint32_t>(AppControllerError::kUnavailable), status_model, settings_store, remote_control_);
        return;
    }
    if (!active_host->Valid()) {
        ESP_LOGE(kTag, "AppController initialization failed");
        RunUnavailableHall(shell_, battery_, wifi_, power_, power_state_, active_host->catalog(),
                           host_ui::HallStatus::kRuntimeUnavailable,
                           static_cast<uint32_t>(AppControllerError::kUnavailable), status_model, settings_store,
                           remote_control_);
        return;
    }
    active_host->Run();
}

}  // namespace micropixel::firmware
