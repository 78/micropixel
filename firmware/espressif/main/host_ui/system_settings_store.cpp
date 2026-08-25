#include "host_ui/system_settings_store.hpp"

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

namespace micropixel::host_ui {
namespace {

constexpr char kTag[] = "micropixel_settings";
constexpr char kPartition[] = "sys_store";
constexpr char kNamespace[] = "system";
constexpr char kControlNamespace[] = "control";
constexpr char kSettingsKey[] = "ui";
constexpr char kControlSettingsKey[] = "settings";
constexpr uint8_t kSettingsVersion = 1U;
constexpr uint8_t kControlSettingsVersion = 1U;
constexpr uint8_t kPerformanceOverlayFlag = 1U << 0U;

struct SettingsRecord final {
    uint8_t version{};
    uint8_t brightness_percent{};
    uint8_t volume_percent{};
    uint8_t flags{};
    uint8_t reserved[4]{};
};

static_assert(sizeof(SettingsRecord) == 8U, "System settings v1 record size changed");

struct ControlSettingsRecord final {
    uint8_t version{};
    uint8_t enabled{};
    uint8_t reserved[6]{};
};

static_assert(sizeof(ControlSettingsRecord) == 8U, "Remote Control settings v1 record size changed");

bool ValidRecord(const SettingsRecord& record) {
    if (record.version != kSettingsVersion || record.brightness_percent > 100U || record.volume_percent > 100U ||
        (record.flags & ~kPerformanceOverlayFlag) != 0U) {
        return false;
    }
    for (uint8_t byte : record.reserved) {
        if (byte != 0U) {
            return false;
        }
    }
    return true;
}

bool ValidControlRecord(const ControlSettingsRecord& record) {
    if (record.version != kControlSettingsVersion || record.enabled > 1U) {
        return false;
    }
    for (uint8_t byte : record.reserved) {
        if (byte != 0U) {
            return false;
        }
    }
    return true;
}

}  // namespace

SystemSettingsStore::~SystemSettingsStore() {
    if (control_handle_ != 0U) {
        nvs_close(control_handle_);
    }
    if (handle_ != 0U) {
        nvs_close(handle_);
    }
}

bool SystemSettingsStore::Initialize() {
    if (handle_ != 0U && control_handle_ != 0U) {
        return true;
    }
    esp_err_t error = nvs_flash_init_partition(kPartition);
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(kTag, "resetting unreadable system settings partition: %s", esp_err_to_name(error));
        error = nvs_flash_erase_partition(kPartition);
        if (error == ESP_OK) {
            error = nvs_flash_init_partition(kPartition);
        }
    }
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "unable to initialize %s: %s", kPartition, esp_err_to_name(error));
        return false;
    }
    error = nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle_);
    if (error != ESP_OK) {
        handle_ = 0U;
        ESP_LOGE(kTag, "unable to open Host settings namespace: %s", esp_err_to_name(error));
        return false;
    }
    error = nvs_open_from_partition(kPartition, kControlNamespace, NVS_READWRITE, &control_handle_);
    if (error != ESP_OK) {
        nvs_close(handle_);
        handle_ = 0U;
        control_handle_ = 0U;
        ESP_LOGE(kTag, "unable to open Remote Control settings namespace: %s", esp_err_to_name(error));
        return false;
    }
    ESP_LOGI(kTag, "Host system settings ready in %s", kPartition);
    return true;
}

bool SystemSettingsStore::Load(StatusLayerModel& model) const {
    if (handle_ == 0U) {
        return false;
    }
    size_t size = 0U;
    esp_err_t error = nvs_get_blob(handle_, kSettingsKey, nullptr, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(kTag, "no saved Host settings; using defaults");
        return true;
    }
    if (error != ESP_OK || size != sizeof(SettingsRecord)) {
        ESP_LOGW(kTag, "ignored unreadable Host settings record");
        return false;
    }
    SettingsRecord record{};
    error = nvs_get_blob(handle_, kSettingsKey, &record, &size);
    if (error != ESP_OK || size != sizeof(record) || !ValidRecord(record)) {
        ESP_LOGW(kTag, "ignored invalid Host settings v1 record");
        return false;
    }
    model.brightness_percent = record.brightness_percent;
    model.volume_percent = record.volume_percent;
    model.performance_overlay_enabled = (record.flags & kPerformanceOverlayFlag) != 0U;
    ESP_LOGI(kTag, "restored Host settings: brightness=%u volume=%u performance=%s",
             static_cast<unsigned>(model.brightness_percent), static_cast<unsigned>(model.volume_percent),
             model.performance_overlay_enabled ? "on" : "off");
    return true;
}

bool SystemSettingsStore::Save(const StatusLayerModel& model) const {
    if (handle_ == 0U) {
        return false;
    }
    const SettingsRecord record{
        .version = kSettingsVersion,
        .brightness_percent = model.brightness_percent,
        .volume_percent = model.volume_percent,
        .flags = static_cast<uint8_t>(model.performance_overlay_enabled ? kPerformanceOverlayFlag : 0U)};
    esp_err_t error = nvs_set_blob(handle_, kSettingsKey, &record, sizeof(record));
    if (error == ESP_OK) {
        error = nvs_commit(handle_);
    }
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "failed to persist Host settings: %s", esp_err_to_name(error));
        return false;
    }
    ESP_LOGI(kTag, "saved Host settings: brightness=%u volume=%u performance=%s",
             static_cast<unsigned>(model.brightness_percent), static_cast<unsigned>(model.volume_percent),
             model.performance_overlay_enabled ? "on" : "off");
    return true;
}

bool SystemSettingsStore::LoadRemoteControl(RemoteControlModel& model) const {
    if (control_handle_ == 0U) {
        return false;
    }
    size_t size = 0U;
    esp_err_t error = nvs_get_blob(control_handle_, kControlSettingsKey, nullptr, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(kTag, "no saved Remote Control settings; using defaults");
        return true;
    }
    if (error != ESP_OK || size != sizeof(ControlSettingsRecord)) {
        ESP_LOGW(kTag, "ignored unreadable Remote Control settings record");
        return false;
    }
    ControlSettingsRecord record{};
    error = nvs_get_blob(control_handle_, kControlSettingsKey, &record, &size);
    if (error != ESP_OK || size != sizeof(record) || !ValidControlRecord(record)) {
        ESP_LOGW(kTag, "ignored invalid Remote Control settings v1 record");
        return false;
    }
    model.enabled = record.enabled != 0U;
    model.connection_state =
        model.enabled ? RemoteControlConnectionState::kWaitingForNetwork : RemoteControlConnectionState::kDisabled;
    ESP_LOGI(kTag, "restored Remote Control setting: enabled=%s", model.enabled ? "yes" : "no");
    return true;
}

bool SystemSettingsStore::SaveRemoteControl(const RemoteControlModel& model) const {
    if (control_handle_ == 0U) {
        return false;
    }
    const ControlSettingsRecord record{.version = kControlSettingsVersion,
                                       .enabled = static_cast<uint8_t>(model.enabled ? 1U : 0U)};
    esp_err_t error = nvs_set_blob(control_handle_, kControlSettingsKey, &record, sizeof(record));
    if (error == ESP_OK) {
        error = nvs_commit(control_handle_);
    }
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "failed to persist Remote Control settings: %s", esp_err_to_name(error));
        return false;
    }
    ESP_LOGI(kTag, "saved Remote Control setting: enabled=%s", model.enabled ? "yes" : "no");
    return true;
}

}  // namespace micropixel::host_ui
