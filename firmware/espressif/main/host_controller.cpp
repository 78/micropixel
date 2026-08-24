#include "host_controller.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <limits>
#include <optional>
#include <utility>

#include "app_controller.hpp"
#include "device/device_services.hpp"
#include "device/wifi.hpp"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "host_ui/system_settings_store.hpp"
#include "host_ui/system_shell.hpp"
#include "runtime/app_runtime.hpp"
#include "runtime/wamr/diagnostics.h"
#include "task_policy.hpp"

namespace micropixel::firmware {
namespace {

constexpr char kTag[] = "micropixel_host";
constexpr TickType_t kCooperativeStopTimeout = pdMS_TO_TICKS(500);
constexpr TickType_t kForcedStopTimeout = pdMS_TO_TICKS(2500);
constexpr int64_t kPerformanceSamplePeriodUs = 500000;
constexpr int64_t kHallWifiRefreshPeriodUs = 2000000;
constexpr TickType_t kWifiScreenPollPeriod = pdMS_TO_TICKS(200);

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

host_ui::HallModel MakeHallModel(const runtime::InstalledAppCatalog& catalog, const device::WifiSnapshot& wifi,
                                 host_ui::HallStatus status, const runtime::AppRunOutcome* outcome = nullptr,
                                 uint32_t detail = 0U, bool launch_enabled = true,
                                 const HallCoverMappings* covers = nullptr,
                                 const std::optional<uint32_t>& suspended_index = std::nullopt,
                                 const host_ui::HallCoverModel* suspended_snapshot = nullptr) {
    host_ui::HallModel model{.app_count = catalog.count,
                             .status_app_id = outcome != nullptr ? outcome->app_id.data() : nullptr,
                             .status = status,
                             .detail = detail,
                             .launch_enabled = launch_enabled,
                             .wifi = MakeHallWifiModel(wifi)};
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
                .cache_key = (static_cast<uint64_t>(catalog.apps[index].store_offset) << 32U) | asset.content_hash};
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
    for (uint32_t index = 0U; index < catalog.count && index < static_cast<uint32_t>(covers.size()); ++index) {
        if (snapshot_index.has_value() && *snapshot_index == index) {
            continue;
        }
        auto mapping_result = runtime::LaunchAssetMapping::Open(catalog.apps[index].store_offset);
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

void RefreshStatusMetrics(host_ui::StatusLayerModel& model, const runtime::InstalledAppCatalog& catalog) {
    const size_t memory_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    const size_t memory_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    model.memory_total_kib = static_cast<uint32_t>(memory_total / 1024U);
    model.memory_used_kib = static_cast<uint32_t>((memory_total - memory_free) / 1024U);

    const esp_partition_t* app_store =
        esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "app_store");
    model.storage_total_kib = app_store != nullptr ? app_store->size / 1024U : 0U;
    uint32_t used_bytes = 0U;
    for (uint32_t index = 0U; index < catalog.count; ++index) {
        const runtime::InstalledApp& app = catalog.apps[index];
        const uint32_t app_end = app.store_offset + app.bundle_size;
        used_bytes = app_end > used_bytes ? app_end : used_bytes;
    }
    model.storage_used_kib = used_bytes / 1024U;
}

void RefreshWifiStatus(host_ui::StatusLayerModel& model, const device::WifiSnapshot& snapshot) {
    model.wifi_available = snapshot.available;
    model.wifi_enabled = snapshot.enabled;
    model.wifi_connected = snapshot.connected;
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
                                             const runtime::InstalledAppCatalog& catalog) {
    return host_ui::SystemMenuModel{
        .language = "English",
        .installed_app_count = catalog.count,
        .wifi_available = status.wifi_available,
        .wifi_enabled = status.wifi_enabled,
        .wifi_connected = status.wifi_connected,
    };
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

host_ui::SystemInformationModel MakeSystemInformationModel() {
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
    return model;
}

host_ui::AppManagementModel MakeAppManagementModel(const runtime::InstalledAppCatalog& catalog, bool launch_available) {
    host_ui::AppManagementModel model{.app_count = catalog.count, .launch_available = launch_available};
    const esp_partition_t* app_store =
        esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "app_store");
    model.storage_total_kib = app_store != nullptr ? app_store->size / 1024U : 0U;
    uint32_t used_bytes = 0U;
    for (uint32_t index = 0U; index < catalog.count; ++index) {
        const runtime::InstalledApp& source = catalog.apps[index];
        model.apps[index] = host_ui::ManagedAppModel{
            .app_id = source.app_id.data(),
            .display_name = source.display_name.data(),
            .bundle_size_kib = source.bundle_size / 1024U,
        };
        used_bytes = std::max(used_bytes, source.store_offset + source.bundle_size);
    }
    model.storage_used_kib = used_bytes / 1024U;
    // The current App Store partition is deliberately read-only. The UI shows
    // why uninstall is unavailable instead of pretending that an NVS
    // tombstone reclaimed Flash space.
    model.uninstall_available = false;
    return model;
}

bool RunStatusLayer(host_ui::SystemShell& shell, AppController* controller, host_ui::StatusLayerModel& model,
                    const runtime::InstalledAppCatalog& catalog, host_ui::SystemSettingsStore& settings_store) {
    if (controller != nullptr) {
        auto suspend_result = controller->Suspend(pdMS_TO_TICKS(500));
        if (!suspend_result) {
            ESP_LOGE(kTag, "failed to suspend App for status layer: error=%u",
                     static_cast<unsigned>(suspend_result.error()));
            return false;
        }
        shell.StopWatchingGuestActions();
    }
    RefreshStatusMetrics(model, catalog);
    auto show_result = shell.ShowStatusLayer(model);
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
    bool close = false;
    bool settings_changed = false;
    while (!close) {
        const TickType_t timeout = model.performance_overlay_enabled ? pdMS_TO_TICKS(20) : portMAX_DELAY;
        const auto action = shell.PollAction(timeout);
        if (!action.has_value()) {
            const int64_t now_us = esp_timer_get_time();
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

    if (settings_changed && settings_store.ready() && !settings_store.Save(model)) {
        ESP_LOGW(kTag, "Host settings changed but could not be persisted");
    }
    shell.LeaveStatusLayer();
    if (controller == nullptr) {
        return true;
    }
    auto resume_result = controller->Resume();
    if (!resume_result) {
        ESP_LOGE(kTag, "failed to resume App after status layer: error=%u",
                 static_cast<unsigned>(resume_result.error()));
        (void)controller->RequestStop();
        return false;
    }
    shell.WatchGuestActions();
    return true;
}

bool RunWifiSettings(host_ui::SystemShell& shell, device::WifiBackend& wifi, host_ui::StatusLayerModel& status_model) {
    device::WifiSnapshot snapshot = wifi.Snapshot();
    RefreshWifiStatus(status_model, snapshot);
    auto show_result = shell.ShowWifiSettings(MakeWifiSettingsModel(snapshot));
    if (!show_result) {
        ESP_LOGE(kTag, "failed to show Wi-Fi settings: error=%u", static_cast<unsigned>(show_result.error()));
        return false;
    }

    if (snapshot.available && snapshot.enabled && !snapshot.scanning) {
        if (const auto scan_result = wifi.RequestScan(); !scan_result) {
            ESP_LOGW(kTag, "initial Wi-Fi scan failed: error=%u", static_cast<unsigned>(scan_result.error()));
        }
    }
    for (;;) {
        const auto action = shell.PollAction(kWifiScreenPollPeriod);
        if (action.has_value()) {
            std::expected<void, device::WifiError> operation{};
            switch (action->type) {
                case host_ui::SystemUiActionType::kCloseWifiSettings:
                case host_ui::SystemUiActionType::kSuspendToHall:
                    snapshot = wifi.Snapshot();
                    RefreshWifiStatus(status_model, snapshot);
                    shell.LeaveWifiSettings();
                    return true;
                case host_ui::SystemUiActionType::kSetWifiEnabled:
                    operation = wifi.SetEnabled(action->value != 0U);
                    if (operation && action->value != 0U) {
                        if (const auto scan_result = wifi.RequestScan(); !scan_result) {
                            ESP_LOGW(kTag, "Wi-Fi scan after enabling failed: error=%u",
                                     static_cast<unsigned>(scan_result.error()));
                        }
                    }
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

        device::WifiSnapshot refreshed = wifi.Snapshot();
        if (!SameWifiSnapshot(snapshot, refreshed)) {
            snapshot = refreshed;
            RefreshWifiStatus(status_model, snapshot);
            shell.UpdateWifiSettings(MakeWifiSettingsModel(snapshot));
        }
    }
}

bool RunSystemInformation(host_ui::SystemShell& shell) {
    const auto show_result = shell.ShowSystemInformation(MakeSystemInformationModel());
    if (!show_result) {
        ESP_LOGE(kTag, "failed to show System Information: error=%u", static_cast<unsigned>(show_result.error()));
        return false;
    }
    for (;;) {
        const auto action = shell.PollAction(portMAX_DELAY);
        if (!action.has_value()) {
            continue;
        }
        if (action->type == host_ui::SystemUiActionType::kCloseSystemInformation ||
            action->type == host_ui::SystemUiActionType::kSuspendToHall) {
            shell.LeaveSystemInformation();
            return true;
        }
        ESP_LOGW(kTag, "ignored action=%u while System Information is visible", static_cast<unsigned>(action->type));
    }
}

bool RunAppManagement(host_ui::SystemShell& shell, const runtime::InstalledAppCatalog& catalog, bool launch_available,
                      std::optional<uint32_t>& launch_request) {
    const auto show_result = shell.ShowAppManagement(MakeAppManagementModel(catalog, launch_available));
    if (!show_result) {
        ESP_LOGE(kTag, "failed to show App Management: error=%u", static_cast<unsigned>(show_result.error()));
        return false;
    }
    for (;;) {
        const auto action = shell.PollAction(portMAX_DELAY);
        if (!action.has_value()) {
            continue;
        }
        if (action->type == host_ui::SystemUiActionType::kCloseAppManagement ||
            action->type == host_ui::SystemUiActionType::kSuspendToHall) {
            shell.LeaveAppManagement();
            return true;
        }
        if (action->type == host_ui::SystemUiActionType::kLaunchManagedApp && launch_available &&
            action->app_index < catalog.count) {
            launch_request = action->app_index;
            shell.LeaveAppManagement();
            return true;
        }
        if (action->type == host_ui::SystemUiActionType::kUninstallManagedApp) {
            ESP_LOGW(kTag, "ignored uninstall request while App Store is read-only: index=%" PRIu32, action->app_index);
            continue;
        }
        ESP_LOGW(kTag, "ignored action=%u while App Management is visible", static_cast<unsigned>(action->type));
    }
}

bool RunSystemMenu(host_ui::SystemShell& shell, device::WifiBackend& wifi, host_ui::StatusLayerModel& status_model,
                   const runtime::InstalledAppCatalog& catalog, host_ui::SystemSettingsStore& settings_store,
                   bool launch_available, std::optional<uint32_t>& launch_request) {
    RefreshWifiStatus(status_model, wifi.Snapshot());
    auto show_result = shell.ShowSystemMenu(MakeSystemMenuModel(status_model, catalog));
    if (!show_result) {
        ESP_LOGE(kTag, "failed to show System Settings: error=%u", static_cast<unsigned>(show_result.error()));
        return false;
    }

    CpuUsageSampler cpu_sampler;
    cpu_sampler.Reset();
    (void)cpu_sampler.Sample();
    int64_t next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
    for (;;) {
        const TickType_t timeout = status_model.performance_overlay_enabled ? pdMS_TO_TICKS(20) : portMAX_DELAY;
        const auto action = shell.PollAction(timeout);
        const int64_t now_us = esp_timer_get_time();
        if (status_model.performance_overlay_enabled && now_us >= next_performance_sample_us) {
            shell.UpdatePerformanceOverlay(true, cpu_sampler.Sample());
            next_performance_sample_us = now_us + kPerformanceSamplePeriodUs;
        }
        if (!action.has_value()) {
            continue;
        }
        switch (action->type) {
            case host_ui::SystemUiActionType::kCloseSystemMenu:
            case host_ui::SystemUiActionType::kSuspendToHall:
                shell.LeaveSystemMenu();
                return true;
            case host_ui::SystemUiActionType::kSelectSystemMenuItem:
                if (action->value == static_cast<uint32_t>(host_ui::SystemMenuItem::kWifi)) {
                    shell.LeaveSystemMenu();
                    if (!RunWifiSettings(shell, wifi, status_model)) {
                        return false;
                    }
                    show_result = shell.ShowSystemMenu(MakeSystemMenuModel(status_model, catalog));
                    if (!show_result) {
                        ESP_LOGE(kTag, "failed to restore System Settings after Wi-Fi: error=%u",
                                 static_cast<unsigned>(show_result.error()));
                        return false;
                    }
                } else if (action->value == static_cast<uint32_t>(host_ui::SystemMenuItem::kSystemInformation)) {
                    shell.LeaveSystemMenu();
                    if (!RunSystemInformation(shell)) {
                        return false;
                    }
                    show_result = shell.ShowSystemMenu(MakeSystemMenuModel(status_model, catalog));
                    if (!show_result) {
                        ESP_LOGE(kTag, "failed to restore System Settings after System Information: error=%u",
                                 static_cast<unsigned>(show_result.error()));
                        return false;
                    }
                } else if (action->value == static_cast<uint32_t>(host_ui::SystemMenuItem::kManageApps)) {
                    shell.LeaveSystemMenu();
                    if (!RunAppManagement(shell, catalog, launch_available, launch_request)) {
                        return false;
                    }
                    if (launch_request.has_value()) {
                        return true;
                    }
                    show_result = shell.ShowSystemMenu(MakeSystemMenuModel(status_model, catalog));
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
                if (!RunStatusLayer(shell, nullptr, status_model, catalog, settings_store)) {
                    return false;
                }
                show_result = shell.ShowSystemMenu(MakeSystemMenuModel(status_model, catalog));
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

void RunUnavailableHall(host_ui::SystemShell& shell, device::WifiBackend& wifi,
                        const runtime::InstalledAppCatalog& catalog, host_ui::HallStatus status, uint32_t detail,
                        host_ui::StatusLayerModel& status_model, host_ui::SystemSettingsStore& settings_store) {
    HallCoverMappings covers = OpenHallCovers(catalog);
    CpuUsageSampler cpu_sampler;
    cpu_sampler.Reset();
    (void)cpu_sampler.Sample();
    int64_t next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
    int64_t next_wifi_refresh_us = esp_timer_get_time() + kHallWifiRefreshPeriodUs;
    for (;;) {
        const device::WifiSnapshot wifi_snapshot = wifi.Snapshot();
        RefreshWifiStatus(status_model, wifi_snapshot);
        if (!ShowHall(shell, MakeHallModel(catalog, wifi_snapshot, status, nullptr, detail, false, &covers))) {
            return;
        }
        shell.UpdatePerformanceOverlay(status_model.performance_overlay_enabled, 0U);
        for (;;) {
            const TickType_t timeout =
                status_model.performance_overlay_enabled ? pdMS_TO_TICKS(20) : pdMS_TO_TICKS(250);
            const auto action = shell.PollAction(timeout);
            const int64_t now_us = esp_timer_get_time();
            if (status_model.performance_overlay_enabled && now_us >= next_performance_sample_us) {
                shell.UpdatePerformanceOverlay(true, cpu_sampler.Sample());
                next_performance_sample_us = now_us + kPerformanceSamplePeriodUs;
            }
            if (now_us >= next_wifi_refresh_us) {
                const device::WifiSnapshot wifi_snapshot = wifi.Snapshot();
                RefreshWifiStatus(status_model, wifi_snapshot);
                shell.UpdateHallWifi(MakeHallWifiModel(wifi_snapshot));
                next_wifi_refresh_us = now_us + kHallWifiRefreshPeriodUs;
            }
            if (!action.has_value()) {
                continue;
            }
            if (action->type == host_ui::SystemUiActionType::kOpenWifiSettings) {
                (void)RunWifiSettings(shell, wifi, status_model);
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                next_wifi_refresh_us = esp_timer_get_time() + kHallWifiRefreshPeriodUs;
                break;
            }
            if (action->type == host_ui::SystemUiActionType::kOpenSystemMenu) {
                std::optional<uint32_t> launch_request;
                (void)RunSystemMenu(shell, wifi, status_model, catalog, settings_store, false, launch_request);
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                next_wifi_refresh_us = esp_timer_get_time() + kHallWifiRefreshPeriodUs;
                break;
            }
            if (action->type != host_ui::SystemUiActionType::kOpenStatusLayer) {
                ESP_LOGW(kTag, "ignored action=%u while App launch is unavailable",
                         static_cast<unsigned>(action->type));
                continue;
            }
            (void)RunStatusLayer(shell, nullptr, status_model, catalog, settings_store);
            cpu_sampler.Reset();
            (void)cpu_sampler.Sample();
            next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
            break;
        }
    }
}

class ActiveHost final {
   public:
    ActiveHost(runtime::InstalledAppCatalog catalog, runtime::AppRuntime& runtime, host_ui::SystemShell& shell,
               device::WifiBackend& wifi, host_ui::StatusLayerModel& status_model,
               host_ui::SystemSettingsStore& settings_store)
        : catalog_(std::move(catalog)),
          app_controller_(runtime),
          shell_(shell),
          wifi_(wifi),
          status_model_(status_model),
          settings_store_(settings_store),
          hall_status_(catalog_.count == 0U ? host_ui::HallStatus::kNoApps : host_ui::HallStatus::kReady) {}

    [[nodiscard]] bool Valid() const { return app_controller_.valid(); }

    void Run() {
        for (;;) {
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

    [[nodiscard]] bool ShowCurrentHall(const HallCoverMappings& covers) {
        const device::WifiSnapshot wifi_snapshot = wifi_.Snapshot();
        RefreshWifiStatus(status_model_, wifi_snapshot);
        if (!ShowHall(shell_, MakeHallModel(catalog_, wifi_snapshot, hall_status_, outcome_, hall_detail_, CanLaunch(),
                                            &covers, suspended_index_, &suspended_snapshot_))) {
            return false;
        }
        shell_.UpdatePerformanceOverlay(status_model_.performance_overlay_enabled, 0U);
        return true;
    }

    void RecordHostFailure(uint32_t detail) {
        outcome_ = nullptr;
        hall_status_ = host_ui::HallStatus::kHostFailure;
        hall_detail_ = detail;
    }

    [[nodiscard]] bool RunHall() {
        const std::optional<uint32_t> snapshot_index =
            suspended_snapshot_.data != nullptr ? suspended_index_ : std::nullopt;
        HallCoverMappings covers = OpenHallCovers(catalog_, snapshot_index);
        if (!ShowCurrentHall(covers)) {
            return false;
        }

        CpuUsageSampler cpu_sampler;
        cpu_sampler.Reset();
        (void)cpu_sampler.Sample();
        int64_t next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
        int64_t next_wifi_refresh_us = esp_timer_get_time() + kHallWifiRefreshPeriodUs;
        for (;;) {
            const TickType_t timeout =
                status_model_.performance_overlay_enabled ? pdMS_TO_TICKS(20) : pdMS_TO_TICKS(250);
            const auto pending_action = shell_.PollAction(timeout);
            const int64_t now_us = esp_timer_get_time();
            if (status_model_.performance_overlay_enabled && now_us >= next_performance_sample_us) {
                shell_.UpdatePerformanceOverlay(true, cpu_sampler.Sample());
                next_performance_sample_us = now_us + kPerformanceSamplePeriodUs;
            }
            if (now_us >= next_wifi_refresh_us) {
                const device::WifiSnapshot wifi_snapshot = wifi_.Snapshot();
                RefreshWifiStatus(status_model_, wifi_snapshot);
                shell_.UpdateHallWifi(MakeHallWifiModel(wifi_snapshot));
                next_wifi_refresh_us = now_us + kHallWifiRefreshPeriodUs;
            }
            if (!pending_action.has_value()) {
                continue;
            }

            const host_ui::SystemUiAction action = *pending_action;
            if (action.type == host_ui::SystemUiActionType::kLaunchApp && action.app_index < catalog_.count &&
                CanLaunch()) {
                ActivateSelectedApp(action.app_index);
                return true;
            }
            if (action.type == host_ui::SystemUiActionType::kStopApp && suspended_index_.has_value() &&
                action.app_index == *suspended_index_) {
                if (!StopSuspendedApp(covers)) {
                    return false;
                }
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                continue;
            }
            if (action.type == host_ui::SystemUiActionType::kOpenStatusLayer) {
                if (!RunStatusLayer(shell_, nullptr, status_model_, catalog_, settings_store_)) {
                    RecordHostFailure(static_cast<uint32_t>(host_ui::SystemUiError::kRenderFailed));
                }
                if (!ShowCurrentHall(covers)) {
                    return false;
                }
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                continue;
            }
            if (action.type == host_ui::SystemUiActionType::kOpenWifiSettings) {
                if (!RunWifiSettings(shell_, wifi_, status_model_)) {
                    RecordHostFailure(static_cast<uint32_t>(host_ui::SystemUiError::kRenderFailed));
                }
                if (!ShowCurrentHall(covers)) {
                    return false;
                }
                cpu_sampler.Reset();
                (void)cpu_sampler.Sample();
                next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
                continue;
            }
            if (action.type == host_ui::SystemUiActionType::kOpenSystemMenu) {
                std::optional<uint32_t> launch_request;
                if (!RunSystemMenu(shell_, wifi_, status_model_, catalog_, settings_store_, CanLaunch(),
                                   launch_request)) {
                    RecordHostFailure(static_cast<uint32_t>(host_ui::SystemUiError::kRenderFailed));
                }
                if (!ShowCurrentHall(covers)) {
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
            ESP_LOGW(kTag, "ignored invalid App Hall action: type=%u index=%" PRIu32,
                     static_cast<unsigned>(action.type), action.app_index);
        }
    }

    [[nodiscard]] bool StopSuspendedApp(HallCoverMappings& covers) {
        const uint32_t stopped_index = *suspended_index_;
        ESP_LOGI(kTag, "stopping suspended App from Hall: index=%" PRIu32, stopped_index);
        auto stopped_result = StopApp(app_controller_);
        if (!stopped_result) {
            ESP_LOGE(kTag, "Hall could not complete suspended App stop: error=%u",
                     static_cast<unsigned>(stopped_result.error()));
            RecordHostFailure(static_cast<uint32_t>(stopped_result.error()));
            return ShowCurrentHall(covers);
        }

        suspended_index_.reset();
        auto stopped_cover = runtime::LaunchAssetMapping::Open(catalog_.apps[stopped_index].store_offset);
        if (stopped_cover) {
            covers[stopped_index] = std::move(*stopped_cover);
        } else {
            ESP_LOGW(kTag, "stopped App cover unavailable: index=%" PRIu32 " app=%s", stopped_index,
                     catalog_.apps[stopped_index].app_id.data());
        }
        outcome_ = nullptr;
        hall_status_ = host_ui::HallStatus::kReady;
        hall_detail_ = 0U;
        if (!ShowCurrentHall(covers)) {
            return false;
        }
        shell_.ReleaseGuestSnapshot();
        suspended_snapshot_ = {};
        micropixel_log_heap_state("host after Hall App stop");
        return true;
    }

    void ActivateSelectedApp(uint32_t selected_index) {
        bool resumed_existing = false;
        if (suspended_index_.has_value()) {
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
                    return;
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
                    return;
                }
                ESP_LOGI(kTag, "resumed selected App: index=%" PRIu32, selected_index);
                resumed_existing = true;
            } else {
                ESP_LOGI(kTag, "switching App: stopping suspended index before launching index=%" PRIu32,
                         selected_index);
                auto stopped_result = StopApp(app_controller_);
                if (!stopped_result) {
                    RecordHostFailure(static_cast<uint32_t>(stopped_result.error()));
                    return;
                }
                micropixel_log_heap_state("host after suspended App stop");
            }
        } else {
            shell_.LeaveHall();
        }

        const runtime::InstalledApp& selected_app = catalog_.apps[selected_index];
        if (!resumed_existing) {
            ESP_LOGI(kTag, "launching selected App: index=%" PRIu32 " app=%s offset=0x%" PRIx32, selected_index,
                     selected_app.app_id.data(), selected_app.store_offset);
            micropixel_log_heap_state("host before AppController start");
            auto start_result = app_controller_.Start(selected_app);
            if (!start_result) {
                ESP_LOGE(kTag, "Host could not start the selected AppSession: error=%u",
                         static_cast<unsigned>(start_result.error()));
                RecordHostFailure(static_cast<uint32_t>(start_result.error()));
                return;
            }
        }

        foreground_index_ = selected_index;
        state_ = State::kForeground;
    }

    void RunForeground() {
        CpuUsageSampler cpu_sampler;
        cpu_sampler.Reset();
        (void)cpu_sampler.Sample();
        int64_t next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
        shell_.UpdatePerformanceOverlay(status_model_.performance_overlay_enabled, 0U);
        shell_.WatchGuestActions();

        for (;;) {
            auto completion_result = app_controller_.PollCompletion(pdMS_TO_TICKS(20));
            if (!completion_result) {
                ESP_LOGE(kTag, "Host could not join the completed AppSession: error=%u",
                         static_cast<unsigned>(completion_result.error()));
                LeaveForegroundUi();
                RecordHostFailure(static_cast<uint32_t>(completion_result.error()));
                state_ = State::kHall;
                return;
            }
            if (completion_result->has_value()) {
                last_outcome_ = **completion_result;
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
                }
                micropixel_log_heap_state("host after AppController completion");
                state_ = State::kHall;
                return;
            }

            const int64_t now_us = esp_timer_get_time();
            if (status_model_.performance_overlay_enabled && now_us >= next_performance_sample_us) {
                shell_.UpdatePerformanceOverlay(true, cpu_sampler.Sample());
                next_performance_sample_us = now_us + kPerformanceSamplePeriodUs;
            }

            const auto action = shell_.PollAction(0U);
            if (!action.has_value()) {
                continue;
            }
            if (action->type == host_ui::SystemUiActionType::kOpenStatusLayer) {
                OpenForegroundStatusLayer(cpu_sampler, next_performance_sample_us);
                continue;
            }
            if (action->type == host_ui::SystemUiActionType::kSuspendToHall &&
                app_controller_.state() == AppLifecycleState::kForeground) {
                SuspendToHall();
                return;
            }
            ESP_LOGW(kTag, "ignored Guest system action=%u in lifecycle state=%u", static_cast<unsigned>(action->type),
                     static_cast<unsigned>(app_controller_.state()));
        }
    }

    void OpenForegroundStatusLayer(CpuUsageSampler& cpu_sampler, int64_t& next_performance_sample_us) {
        if (app_controller_.state() != AppLifecycleState::kForeground) {
            ESP_LOGW(kTag, "ignored status-layer gesture in lifecycle state=%u",
                     static_cast<unsigned>(app_controller_.state()));
            return;
        }
        if (!RunStatusLayer(shell_, &app_controller_, status_model_, catalog_, settings_store_)) {
            RecordHostFailure(static_cast<uint32_t>(AppControllerError::kResumeFailed));
        }
        cpu_sampler.Reset();
        (void)cpu_sampler.Sample();
        next_performance_sample_us = esp_timer_get_time() + kPerformanceSamplePeriodUs;
    }

    void SuspendToHall() {
        auto suspend_result = app_controller_.Suspend(pdMS_TO_TICKS(500));
        if (!suspend_result) {
            ESP_LOGE(kTag, "failed to suspend App for Hall: error=%u", static_cast<unsigned>(suspend_result.error()));
            RecordHostFailure(static_cast<uint32_t>(suspend_result.error()));
            return;
        }
        shell_.StopWatchingGuestActions();
        auto snapshot_result = shell_.CaptureGuestFrame();
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
        LeaveForegroundUi();
        ESP_LOGI(kTag, "suspended App moved to Hall: index=%" PRIu32, foreground_index_);
        state_ = State::kHall;
    }

    void LeaveForegroundUi() {
        shell_.StopWatchingGuestActions();
        shell_.UpdatePerformanceOverlay(false, 0U);
    }

    runtime::InstalledAppCatalog catalog_;
    AppController app_controller_;
    host_ui::SystemShell& shell_;
    device::WifiBackend& wifi_;
    host_ui::StatusLayerModel& status_model_;
    host_ui::SystemSettingsStore& settings_store_;
    State state_{State::kHall};
    runtime::AppRunOutcome last_outcome_{};
    const runtime::AppRunOutcome* outcome_{};
    host_ui::HallStatus hall_status_;
    uint32_t hall_detail_{};
    uint32_t foreground_index_{};
    std::optional<uint32_t> suspended_index_;
    host_ui::HallCoverModel suspended_snapshot_{};
};

}  // namespace

HostController::HostController(device::DeviceServices& devices, device::WifiBackend& wifi, host_ui::SystemShell& shell)
    : devices_(devices), wifi_(wifi), shell_(shell) {}

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
    RefreshWifiStatus(status_model, wifi_.Snapshot());
    shell_.ApplyBrightness(status_model.brightness_percent);
    shell_.ApplyVolume(status_model.volume_percent);

    auto catalog_result = runtime::ScanInstalledApps();
    if (!catalog_result) {
        ESP_LOGE(kTag, "App Store catalog scan failed");
        const runtime::InstalledAppCatalog empty_catalog{};
        RunUnavailableHall(shell_, wifi_, empty_catalog, host_ui::HallStatus::kNoApps,
                           static_cast<uint32_t>(catalog_result.error()), status_model, settings_store);
        return;
    }
    runtime::InstalledAppCatalog catalog = *catalog_result;

    auto runtime_result = runtime::AppRuntime::Initialize(devices_);
    if (!runtime_result) {
        ESP_LOGE(kTag, "AppRuntime initialization failed: error=%u", static_cast<unsigned>(runtime_result.error()));
        RunUnavailableHall(shell_, wifi_, catalog, host_ui::HallStatus::kRuntimeUnavailable,
                           static_cast<uint32_t>(runtime_result.error()), status_model, settings_store);
        return;
    }

    runtime::AppRuntime app_runtime = std::move(*runtime_result);
    ActiveHost active_host(catalog, app_runtime, shell_, wifi_, status_model, settings_store);
    if (!active_host.Valid()) {
        ESP_LOGE(kTag, "AppController initialization failed");
        RunUnavailableHall(shell_, wifi_, catalog, host_ui::HallStatus::kRuntimeUnavailable,
                           static_cast<uint32_t>(AppControllerError::kUnavailable), status_model, settings_store);
        return;
    }
    active_host.Run();
}

}  // namespace micropixel::firmware
