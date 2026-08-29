#include "host/ui/system_settings_store.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

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
constexpr char kLocaleSettingsKey[] = "locale";
constexpr char kControlSettingsKey[] = "settings";
constexpr uint8_t kSettingsVersion = 3U;
constexpr uint8_t kLocaleSettingsVersion = 1U;
constexpr uint8_t kControlSettingsVersion = 1U;
constexpr uint8_t kPerformanceOverlayFlag = 1U << 0U;

struct SettingsRecord final {
    uint8_t version{};
    uint8_t brightness_percent{};
    uint8_t volume_percent{};
    uint8_t flags{};
    uint8_t auto_sleep_timeout_minutes{};
    uint8_t theme_mode{};
    uint8_t reserved[2]{};
};

static_assert(sizeof(SettingsRecord) == 8U, "System settings record size changed");

struct LocaleSettingsRecord final {
    uint8_t version{};
    uint8_t requested_length{};
    uint8_t reserved[2]{};
    char requested[MICROPIXEL_LOCALE_TAG_MAX_BYTES + 1U]{};
};

static_assert(sizeof(LocaleSettingsRecord) == 36U, "Locale settings v1 record size changed");

struct ControlSettingsRecord final {
    uint8_t version{};
    uint8_t enabled{};
    uint8_t reserved[6]{};
};

static_assert(sizeof(ControlSettingsRecord) == 8U, "Remote Control settings v1 record size changed");

bool ValidRecord(const SettingsRecord& record) {
    if (record.version != kSettingsVersion || record.brightness_percent > 100U || record.volume_percent > 100U ||
        (record.flags & ~kPerformanceOverlayFlag) != 0U ||
        (record.auto_sleep_timeout_minutes != 0U && record.auto_sleep_timeout_minutes != 1U &&
         record.auto_sleep_timeout_minutes != 5U && record.auto_sleep_timeout_minutes != 10U &&
         record.auto_sleep_timeout_minutes != 30U) ||
        record.theme_mode > static_cast<uint8_t>(SystemThemeMode::kSoftIvory)) {
        return false;
    }
    for (uint8_t byte : record.reserved) {
        if (byte != 0U) {
            return false;
        }
    }
    return true;
}

bool ValidVersion2Record(const SettingsRecord& record) {
    if (record.version != 2U || record.brightness_percent > 100U || record.volume_percent > 100U ||
        (record.flags & ~kPerformanceOverlayFlag) != 0U ||
        (record.auto_sleep_timeout_minutes != 0U && record.auto_sleep_timeout_minutes != 1U &&
         record.auto_sleep_timeout_minutes != 5U && record.auto_sleep_timeout_minutes != 10U &&
         record.auto_sleep_timeout_minutes != 30U) ||
        record.theme_mode != 0U) {
        return false;
    }
    for (uint8_t byte : record.reserved) {
        if (byte != 0U) {
            return false;
        }
    }
    return true;
}

bool ValidLegacyRecord(const SettingsRecord& record) {
    if (record.version != 1U || record.brightness_percent > 100U || record.volume_percent > 100U ||
        (record.flags & ~kPerformanceOverlayFlag) != 0U || record.auto_sleep_timeout_minutes != 0U ||
        record.theme_mode != 0U) {
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

bool ValidLocaleRecord(const LocaleSettingsRecord& record) {
    if (record.version != kLocaleSettingsVersion || record.requested_length == 0U ||
        record.requested_length > MICROPIXEL_LOCALE_TAG_MAX_BYTES || record.reserved[0] != 0U ||
        record.reserved[1] != 0U || record.requested[record.requested_length] != '\0') {
        return false;
    }
    for (size_t index = static_cast<size_t>(record.requested_length) + 1U; index < sizeof(record.requested); ++index) {
        if (record.requested[index] != '\0') {
            return false;
        }
    }
    LocaleTagBuffer normalized{};
    const std::string_view requested(record.requested, record.requested_length);
    return NormalizeLocaleTag(requested, normalized) && std::string_view(normalized.data()) == requested;
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
    const bool version1 = ValidLegacyRecord(record);
    const bool version2 = ValidVersion2Record(record);
    if (error != ESP_OK || size != sizeof(record) || (!version1 && !version2 && !ValidRecord(record))) {
        ESP_LOGW(kTag, "ignored invalid Host settings record");
        return false;
    }
    model.brightness_percent = record.brightness_percent;
    model.volume_percent = record.volume_percent;
    model.performance_overlay_enabled = (record.flags & kPerformanceOverlayFlag) != 0U;
    model.auto_sleep_timeout_minutes = version1 ? kDefaultAutoSleepTimeoutMinutes : record.auto_sleep_timeout_minutes;
    model.theme_mode =
        version1 || version2 ? SystemThemeMode::kPureBlack : static_cast<SystemThemeMode>(record.theme_mode);
    ESP_LOGI(kTag, "restored Host settings: brightness=%u volume=%u performance=%s auto-sleep=%u min theme=%u",
             static_cast<unsigned>(model.brightness_percent), static_cast<unsigned>(model.volume_percent),
             model.performance_overlay_enabled ? "on" : "off", static_cast<unsigned>(model.auto_sleep_timeout_minutes),
             static_cast<unsigned>(model.theme_mode));
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
        .flags = static_cast<uint8_t>(model.performance_overlay_enabled ? kPerformanceOverlayFlag : 0U),
        .auto_sleep_timeout_minutes = model.auto_sleep_timeout_minutes,
        .theme_mode = static_cast<uint8_t>(model.theme_mode)};
    esp_err_t error = nvs_set_blob(handle_, kSettingsKey, &record, sizeof(record));
    if (error == ESP_OK) {
        error = nvs_commit(handle_);
    }
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "failed to persist Host settings: %s", esp_err_to_name(error));
        return false;
    }
    ESP_LOGI(kTag, "saved Host settings: brightness=%u volume=%u performance=%s auto-sleep=%u min theme=%u",
             static_cast<unsigned>(model.brightness_percent), static_cast<unsigned>(model.volume_percent),
             model.performance_overlay_enabled ? "on" : "off", static_cast<unsigned>(model.auto_sleep_timeout_minutes),
             static_cast<unsigned>(model.theme_mode));
    return true;
}

bool SystemSettingsStore::LoadLocale(SystemLocaleState& locale) const {
    if (handle_ == 0U) {
        return false;
    }
    size_t size = 0U;
    esp_err_t error = nvs_get_blob(handle_, kLocaleSettingsKey, nullptr, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(kTag, "no saved Locale; using %s", locale.requested_c_str());
        return true;
    }
    if (error != ESP_OK || size != sizeof(LocaleSettingsRecord)) {
        ESP_LOGW(kTag, "ignored unreadable Locale settings record");
        return false;
    }
    LocaleSettingsRecord record{};
    error = nvs_get_blob(handle_, kLocaleSettingsKey, &record, &size);
    if (error != ESP_OK || size != sizeof(record) || !ValidLocaleRecord(record) ||
        !locale.SetRequested(std::string_view(record.requested, record.requested_length))) {
        ESP_LOGW(kTag, "ignored invalid Locale settings v1 record");
        return false;
    }
    ESP_LOGI(kTag, "restored requested Locale: %s", locale.requested_c_str());
    return true;
}

bool SystemSettingsStore::SaveLocale(const SystemLocaleState& locale) const {
    if (handle_ == 0U) {
        return false;
    }
    const std::string_view requested = locale.requested();
    LocaleSettingsRecord record{
        .version = kLocaleSettingsVersion,
        .requested_length = static_cast<uint8_t>(requested.size()),
    };
    std::memcpy(record.requested, requested.data(), requested.size());
    esp_err_t error = nvs_set_blob(handle_, kLocaleSettingsKey, &record, sizeof(record));
    if (error == ESP_OK) {
        error = nvs_commit(handle_);
    }
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "failed to persist requested Locale: %s", esp_err_to_name(error));
        return false;
    }
    ESP_LOGI(kTag, "saved requested Locale: %s", locale.requested_c_str());
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
