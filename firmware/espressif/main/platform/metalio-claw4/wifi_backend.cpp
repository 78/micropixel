#include "platform/metalio-claw4/wifi_backend.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstring>

#include "esp_err.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs.h"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "micropixel_wifi";
constexpr char kNvsPartition[] = "runtime_nvs";
constexpr char kNvsNamespace[] = "host_wifi";
constexpr char kNvsStateKey[] = "state";
constexpr uint32_t kStoredStateMagic = 0x57494649U;
constexpr uint16_t kStoredStateVersion = 1U;
constexpr uint8_t kReconnectAttemptLimit = 2U;
constexpr uint16_t kQuickScanMinTimeMs = 20U;
constexpr uint16_t kQuickScanMaxTimeMs = 45U;
constexpr uint8_t kQuickScanHomeDwellTimeMs = 30U;

struct StoredProfile final {
    char ssid[device::kWifiSsidCapacity + 1U]{};
    char password[device::kWifiPasswordCapacity + 1U]{};
    uint8_t bssid[6]{};
    uint8_t channel{};
    uint8_t secured{};
    uint8_t bssid_valid{};
};

struct StoredState final {
    uint32_t magic{kStoredStateMagic};
    uint16_t version{kStoredStateVersion};
    uint8_t profile_count{};
    uint8_t last_profile{};
    uint8_t enabled{1U};
    uint8_t reserved[3]{};
    StoredProfile profiles[device::kMaxSavedWifiNetworks]{};
};

template <size_t Capacity>
bool CopyText(std::array<char, Capacity>& destination, std::string_view source) {
    if (source.empty() || source.size() >= Capacity) {
        return false;
    }
    destination.fill('\0');
    std::memcpy(destination.data(), source.data(), source.size());
    return true;
}

template <size_t Capacity>
void CopyCString(std::array<char, Capacity>& destination, const uint8_t* source, size_t source_capacity) {
    destination.fill('\0');
    if (source == nullptr || source_capacity == 0U) {
        return;
    }
    const size_t length = strnlen(reinterpret_cast<const char*>(source), source_capacity);
    std::memcpy(destination.data(), source, std::min(length, Capacity - 1U));
}

device::WifiBand BandForChannel(uint8_t channel) {
    if (channel >= 1U && channel <= 14U) {
        return device::WifiBand::k2_4Ghz;
    }
    return channel >= 30U ? device::WifiBand::k5Ghz : device::WifiBand::kUnknown;
}

bool SameSsid(const std::array<char, device::kWifiSsidCapacity + 1U>& left, std::string_view right) {
    return std::string_view(left.data()) == right;
}

class ScopedLock final {
   public:
    explicit ScopedLock(SemaphoreHandle_t mutex) : mutex_(mutex) {
        if (mutex_ != nullptr) {
            locked_ = xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
        }
    }
    ~ScopedLock() {
        if (locked_) {
            xSemaphoreGive(mutex_);
        }
    }
    [[nodiscard]] bool locked() const { return locked_; }

   private:
    SemaphoreHandle_t mutex_{};
    bool locked_{};
};

device::WifiError WifiErrorFor(esp_err_t status) {
    if (status == ESP_ERR_INVALID_ARG) {
        return device::WifiError::kInvalidArgument;
    }
    if (status == ESP_ERR_WIFI_STATE || status == ESP_ERR_INVALID_STATE) {
        return device::WifiError::kBusy;
    }
    return device::WifiError::kOperationFailed;
}

esp_err_t StartWifiScan(bool quick) {
    wifi_scan_config_t config{};
    config.show_hidden = false;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    if (quick) {
        config.scan_time.active.min = kQuickScanMinTimeMs;
        config.scan_time.active.max = kQuickScanMaxTimeMs;
        config.home_chan_dwell_time = kQuickScanHomeDwellTimeMs;
    }
    return esp_wifi_scan_start(&config, false);
}

device::WifiConnectionState ConnectionStateForDisconnectReason(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        case WIFI_REASON_802_1X_AUTH_FAILED:
        case WIFI_REASON_AUTH_FAIL:
            return device::WifiConnectionState::kAuthenticationFailed;
        case WIFI_REASON_AUTH_EXPIRE:
            return device::WifiConnectionState::kAuthenticationTimedOut;
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return device::WifiConnectionState::kHandshakeTimedOut;
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return device::WifiConnectionState::kNetworkNotFound;
        default:
            return device::WifiConnectionState::kFailed;
    }
}

const char* AuthModeName(uint8_t auth_mode) {
    switch (static_cast<wifi_auth_mode_t>(auth_mode)) {
        case WIFI_AUTH_OPEN:
            return "OPEN";
        case WIFI_AUTH_WEP:
            return "WEP";
        case WIFI_AUTH_WPA_PSK:
            return "WPA-PSK";
        case WIFI_AUTH_WPA2_PSK:
            return "WPA2-PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "WPA/WPA2-PSK";
        case WIFI_AUTH_WPA3_PSK:
            return "WPA3-SAE";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "WPA2/WPA3";
        case WIFI_AUTH_OWE:
            return "OWE";
        case WIFI_AUTH_UNKNOWN:
            return "UNKNOWN";
        default:
            return "OTHER";
    }
}

void LogCoprocessorInfo() {
    esp_hosted_coprocessor_fwver_t version{};
    const esp_err_t version_status =
        static_cast<esp_err_t>(esp_hosted_get_coprocessor_fwversion(&version));
    if (version_status == ESP_OK) {
        ESP_LOGI(kTag, "ESP-Hosted coprocessor firmware: %" PRIu32 ".%" PRIu32 ".%" PRIu32 " rev=%" PRId32,
                 version.major1, version.minor1, version.patch1, version.revision);
    } else {
        ESP_LOGW(kTag, "could not read ESP-Hosted coprocessor firmware version: %s",
                 esp_err_to_name(version_status));
    }

    uint32_t chip_id = 0U;
    char target[32]{};
    const esp_err_t info_status =
        static_cast<esp_err_t>(esp_hosted_get_cp_info(&chip_id, target, sizeof(target)));
    if (info_status == ESP_OK) {
        ESP_LOGI(kTag, "ESP-Hosted coprocessor: target=%s chip_id=0x%08" PRIx32,
                 target[0] == '\0' ? "unknown" : target, chip_id);
    } else {
        ESP_LOGW(kTag, "could not read ESP-Hosted coprocessor info: %s", esp_err_to_name(info_status));
    }
}

}  // namespace

std::expected<void, device::WifiError> WifiBackend::Initialize() {
    if (initialized_) {
        return {};
    }
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
        return std::unexpected(device::WifiError::kOperationFailed);
    }
    (void)LoadSettings();

    const esp_err_t hosted_status = static_cast<esp_err_t>(esp_hosted_init());
    if (hosted_status != ESP_OK) {
        ESP_LOGW(kTag, "ESP-Hosted initialization failed: %s", esp_err_to_name(hosted_status));
        return std::unexpected(WifiErrorFor(hosted_status));
    }
    esp_err_t status = esp_netif_init();
    if (status != ESP_OK && status != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "esp_netif initialization failed: %s", esp_err_to_name(status));
        return std::unexpected(WifiErrorFor(status));
    }
    status = esp_event_loop_create_default();
    if (status != ESP_OK && status != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "default event loop creation failed: %s", esp_err_to_name(status));
        return std::unexpected(WifiErrorFor(status));
    }
    station_netif_ = esp_netif_create_default_wifi_sta();
    if (station_netif_ == nullptr) {
        ESP_LOGW(kTag, "default station netif creation failed");
        return std::unexpected(device::WifiError::kOperationFailed);
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    status = esp_wifi_init(&config);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "Wi-Fi initialization failed: %s", esp_err_to_name(status));
        return std::unexpected(WifiErrorFor(status));
    }
    status = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (status == ESP_OK) {
        status = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (status == ESP_OK) {
        status = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, WifiEventHandler, this,
                                                     &wifi_event_instance_);
    }
    if (status == ESP_OK) {
        status = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, WifiEventHandler, this,
                                                     &ip_event_instance_);
    }
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "Wi-Fi event setup failed: %s", esp_err_to_name(status));
        return std::unexpected(WifiErrorFor(status));
    }

    {
        ScopedLock lock(mutex_);
        initialized_ = true;
        snapshot_.available = true;
        snapshot_.enabled = enabled_;
        RebuildSnapshotLocked();
    }
    if (enabled_) {
        status = esp_wifi_start();
        if (status != ESP_OK) {
            ESP_LOGW(kTag, "Wi-Fi start failed: %s", esp_err_to_name(status));
            return std::unexpected(WifiErrorFor(status));
        }
#if CONFIG_ESP_HOSTED_CP_TARGET_ESP32C5
        status = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
        if (status != ESP_OK) {
            ESP_LOGW(kTag, "could not enable automatic 2.4/5 GHz band selection: %s", esp_err_to_name(status));
            return std::unexpected(WifiErrorFor(status));
        }
        LogCoprocessorInfo();
#endif
    }
    ESP_LOGI(kTag, "Wi-Fi backend ready: enabled=%s saved=%lu", enabled_ ? "yes" : "no",
             static_cast<unsigned long>(profile_count_));
    return {};
}

device::WifiSnapshot WifiBackend::Snapshot() const {
    device::WifiSnapshot result{};
    {
        ScopedLock lock(mutex_);
        if (lock.locked()) {
            result = snapshot_;
        }
    }
    if (result.connected) {
        wifi_ap_record_t access_point{};
        if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
            for (uint32_t index = 0U; index < result.saved_network_count; ++index) {
                if (result.saved_networks[index].connected) {
                    result.saved_networks[index].rssi = access_point.rssi;
                    result.saved_networks[index].channel = access_point.primary;
                    result.saved_networks[index].band = BandForChannel(access_point.primary);
                    break;
                }
            }
        }
    }
    return result;
}

std::expected<void, device::WifiError> WifiBackend::SetEnabled(bool enabled) {
    {
        ScopedLock lock(mutex_);
        if (!initialized_) {
            return std::unexpected(device::WifiError::kUnavailable);
        }
        if (enabled_ == enabled) {
            return {};
        }
        enabled_ = enabled;
        user_requested_disconnect_ = !enabled;
        associated_ = false;
        reconnect_attempts_ = 0U;
        pending_profile_ = {};
        pending_profile_valid_ = false;
        ignore_next_disconnect_ = false;
        scan_followup_pending_ = false;
        snapshot_.enabled = enabled;
        snapshot_.connected = false;
        snapshot_.scanning = false;
        snapshot_.connection_state = device::WifiConnectionState::kDisconnected;
        RebuildSnapshotLocked();
    }

    esp_err_t status = enabled ? esp_wifi_start() : esp_wifi_stop();
#if CONFIG_ESP_HOSTED_CP_TARGET_ESP32C5
    if (status == ESP_OK && enabled) {
        status = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    }
#endif
    if (status != ESP_OK) {
        ScopedLock lock(mutex_);
        enabled_ = !enabled;
        snapshot_.enabled = enabled_;
        snapshot_.connection_state = device::WifiConnectionState::kFailed;
        RebuildSnapshotLocked();
        return std::unexpected(WifiErrorFor(status));
    }
    (void)SaveSettings();
    return {};
}

std::expected<void, device::WifiError> WifiBackend::RequestScan() {
    {
        ScopedLock lock(mutex_);
        if (!initialized_ || !enabled_) {
            return std::unexpected(device::WifiError::kUnavailable);
        }
        if (snapshot_.scanning) {
            return std::unexpected(device::WifiError::kBusy);
        }
    }
    const esp_err_t status = StartWifiScan(true);
    if (status != ESP_OK) {
        return std::unexpected(WifiErrorFor(status));
    }
    ScopedLock lock(mutex_);
    snapshot_.scanning = true;
    scan_followup_pending_ = true;
    return {};
}

std::expected<void, device::WifiError> WifiBackend::ConnectSaved(std::string_view ssid) {
    SavedProfile profile{};
    int32_t index = -1;
    bool have_target = false;
    {
        ScopedLock lock(mutex_);
        if (!initialized_ || !enabled_) {
            return std::unexpected(device::WifiError::kUnavailable);
        }
        index = FindSavedProfile(ssid);
        if (index < 0) {
            return std::unexpected(device::WifiError::kInvalidArgument);
        }
        profile = profiles_[static_cast<uint32_t>(index)];
        have_target = ApplyScanTarget(ssid, profile) || (profile.channel != 0U && profile.bssid_valid);
        active_profile_index_ = index;
        pending_profile_valid_ = false;
        user_requested_disconnect_ = false;
        reconnect_attempts_ = 0U;
    }
    return ConfigureAndConnect(profile, !have_target);
}

std::expected<void, device::WifiError> WifiBackend::Connect(std::string_view ssid, std::string_view password) {
    if (ssid.empty() || ssid.size() > device::kWifiSsidCapacity || password.size() > device::kWifiPasswordCapacity) {
        return std::unexpected(device::WifiError::kInvalidArgument);
    }
    SavedProfile profile{};
    if (!CopyText(profile.ssid, ssid)) {
        return std::unexpected(device::WifiError::kInvalidArgument);
    }
    if (!password.empty() && !CopyText(profile.password, password)) {
        return std::unexpected(device::WifiError::kInvalidArgument);
    }
    profile.secured = !password.empty();

    bool have_target = false;
    {
        ScopedLock lock(mutex_);
        if (!initialized_ || !enabled_) {
            return std::unexpected(device::WifiError::kUnavailable);
        }
        const int32_t index = FindSavedProfile(ssid);
        if (index < 0 && profile_count_ >= profiles_.size()) {
            return std::unexpected(device::WifiError::kStorage);
        }
        if (index >= 0) {
            const SavedProfile& saved = profiles_[static_cast<uint32_t>(index)];
            profile.channel = saved.channel;
            profile.bssid = saved.bssid;
            profile.bssid_valid = saved.bssid_valid;
        }
        have_target = ApplyScanTarget(ssid, profile) || (profile.channel != 0U && profile.bssid_valid);
        pending_profile_ = profile;
        pending_profile_valid_ = true;
        active_profile_index_ = index;
        user_requested_disconnect_ = false;
        reconnect_attempts_ = 0U;
    }
    return ConfigureAndConnect(profile, !have_target);
}

std::expected<void, device::WifiError> WifiBackend::Disconnect() {
    {
        ScopedLock lock(mutex_);
        if (!initialized_ || !enabled_) {
            return std::unexpected(device::WifiError::kUnavailable);
        }
        user_requested_disconnect_ = true;
        associated_ = false;
        reconnect_attempts_ = 0U;
        pending_profile_ = {};
        pending_profile_valid_ = false;
        ignore_next_disconnect_ = false;
        snapshot_.connected = false;
        snapshot_.connection_state = device::WifiConnectionState::kDisconnected;
        RebuildSnapshotLocked();
    }
    const esp_err_t status = esp_wifi_disconnect();
    if (status != ESP_OK && status != ESP_ERR_WIFI_NOT_CONNECT) {
        return std::unexpected(WifiErrorFor(status));
    }
    return {};
}

std::expected<void, device::WifiError> WifiBackend::Forget(std::string_view ssid) {
    bool disconnect = false;
    {
        ScopedLock lock(mutex_);
        const int32_t index = FindSavedProfile(ssid);
        if (index < 0) {
            return std::unexpected(device::WifiError::kInvalidArgument);
        }
        disconnect = active_profile_index_ == index;
        for (uint32_t current = static_cast<uint32_t>(index); current + 1U < profile_count_; ++current) {
            profiles_[current] = profiles_[current + 1U];
        }
        if (profile_count_ > 0U) {
            profiles_[--profile_count_] = {};
        }
        if (disconnect) {
            active_profile_index_ = -1;
            user_requested_disconnect_ = true;
            associated_ = false;
            reconnect_attempts_ = 0U;
            pending_profile_ = {};
            pending_profile_valid_ = false;
            snapshot_.connected = false;
            snapshot_.connection_state = device::WifiConnectionState::kDisconnected;
        } else if (active_profile_index_ > index) {
            --active_profile_index_;
        }
        RebuildSnapshotLocked();
    }
    if (disconnect) {
        (void)esp_wifi_disconnect();
    }
    return SaveSettings() ? std::expected<void, device::WifiError>{} : std::unexpected(device::WifiError::kStorage);
}

void WifiBackend::WifiEventHandler(void* context, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* backend = static_cast<WifiBackend*>(context);
    if (backend == nullptr) {
        return;
    }
    if (event_base == WIFI_EVENT) {
        backend->HandleWifiEvent(event_id, event_data);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        backend->HandleGotIp();
    }
}

void WifiBackend::HandleWifiEvent(int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_SCAN_DONE) {
        HandleScanDone();
        return;
    }
    if (event_id == WIFI_EVENT_STA_START) {
        SavedProfile profile{};
        bool should_connect = false;
        {
            ScopedLock lock(mutex_);
            should_connect = enabled_ && profile_count_ > 0U;
            if (should_connect) {
                if (active_profile_index_ < 0 || active_profile_index_ >= static_cast<int32_t>(profile_count_)) {
                    active_profile_index_ = 0;
                }
                profile = profiles_[static_cast<uint32_t>(active_profile_index_)];
                user_requested_disconnect_ = false;
                reconnect_attempts_ = 0U;
            }
        }
        if (should_connect) {
            (void)ConfigureAndConnect(profile, false);
        }
        return;
    }
    if (event_id == WIFI_EVENT_STA_CONNECTED) {
        const auto* connected = static_cast<const wifi_event_sta_connected_t*>(event_data);
        if (connected == nullptr) {
            return;
        }
        {
            ScopedLock lock(mutex_);
            associated_ = true;
            reconnect_attempts_ = 0U;
            if (pending_profile_valid_) {
                int32_t pending_index = FindSavedProfile(pending_profile_.ssid.data());
                if (pending_index < 0 && profile_count_ < profiles_.size()) {
                    pending_index = static_cast<int32_t>(profile_count_++);
                }
                if (pending_index >= 0) {
                    profiles_[static_cast<uint32_t>(pending_index)] = pending_profile_;
                    active_profile_index_ = pending_index;
                }
                pending_profile_ = {};
                pending_profile_valid_ = false;
            }
            if (active_profile_index_ >= 0 && active_profile_index_ < static_cast<int32_t>(profile_count_)) {
                SavedProfile& profile = profiles_[static_cast<uint32_t>(active_profile_index_)];
                profile.channel = connected->channel;
                std::memcpy(profile.bssid.data(), connected->bssid, profile.bssid.size());
                profile.bssid_valid = true;
            }
            snapshot_.connection_state = device::WifiConnectionState::kConnecting;
            RebuildSnapshotLocked();
        }
        (void)SaveSettings();
        return;
    }
    if (event_id != WIFI_EVENT_STA_DISCONNECTED) {
        return;
    }

    const auto* disconnected = static_cast<const wifi_event_sta_disconnected_t*>(event_data);
    const uint8_t disconnect_reason =
        disconnected == nullptr ? static_cast<uint8_t>(WIFI_REASON_UNSPECIFIED) : disconnected->reason;
    if (disconnected != nullptr) {
        ESP_LOGW(kTag,
                 "station disconnected: reason=%u ssid=%.*s rssi=%d bssid=%02x:%02x:%02x:%02x:%02x:%02x",
                 disconnect_reason, static_cast<int>(disconnected->ssid_len),
                 reinterpret_cast<const char*>(disconnected->ssid), disconnected->rssi, disconnected->bssid[0],
                 disconnected->bssid[1], disconnected->bssid[2], disconnected->bssid[3], disconnected->bssid[4],
                 disconnected->bssid[5]);
    } else {
        ESP_LOGW(kTag, "station disconnected without event data: reason=%u", disconnect_reason);
    }
    bool should_retry = false;
    bool user_connection_failed = false;
    SavedProfile retry_profile{};
    uint8_t retry_attempt = 0U;
    {
        ScopedLock lock(mutex_);
        associated_ = false;
        snapshot_.connected = false;
        if (ignore_next_disconnect_) {
            ignore_next_disconnect_ = false;
            RebuildSnapshotLocked();
            return;
        }
        if (user_requested_disconnect_) {
            snapshot_.connection_state = device::WifiConnectionState::kDisconnected;
            RebuildSnapshotLocked();
            return;
        }
        user_connection_failed = pending_profile_valid_;
        if (user_connection_failed) {
            pending_profile_ = {};
            pending_profile_valid_ = false;
            snapshot_.connection_state = ConnectionStateForDisconnectReason(disconnect_reason);
        } else {
            snapshot_.connection_state = device::WifiConnectionState::kFailed;
        }
        should_retry = enabled_ && !user_requested_disconnect_ && !user_connection_failed &&
                       active_profile_index_ >= 0 && reconnect_attempts_ < kReconnectAttemptLimit;
        if (should_retry) {
            ++reconnect_attempts_;
            retry_attempt = reconnect_attempts_;
            retry_profile = profiles_[static_cast<uint32_t>(active_profile_index_)];
        }
        RebuildSnapshotLocked();
    }
    if (user_connection_failed) {
        ESP_LOGW(kTag, "new Wi-Fi connection failed: reason=%u (%s)", disconnect_reason,
                 disconnect_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
                     ? "AP-reported 4-way handshake timeout"
                 : disconnect_reason == WIFI_REASON_HANDSHAKE_TIMEOUT ? "local handshake timeout"
                                                                     : "see wifi_err_reason_t");
    }
    if (should_retry) {
        const auto result = ConfigureAndConnect(retry_profile, false);
        ESP_LOGI(kTag, "known-channel reconnect attempt %u: %s", retry_attempt, result ? "started" : "failed");
    }
}

void WifiBackend::HandleGotIp() {
    ScopedLock lock(mutex_);
    if (!associated_) {
        return;
    }
    snapshot_.connected = true;
    snapshot_.connection_state = device::WifiConnectionState::kConnected;
    reconnect_attempts_ = 0U;
    RebuildSnapshotLocked();
}

void WifiBackend::HandleScanDone() {
    std::array<wifi_ap_record_t, device::kMaxVisibleWifiNetworks> records{};
    uint16_t count = static_cast<uint16_t>(records.size());
    const esp_err_t status = esp_wifi_scan_get_ap_records(&count, records.data());
    bool start_followup = false;
    {
        ScopedLock lock(mutex_);
        start_followup = scan_followup_pending_;
        scan_followup_pending_ = false;
        snapshot_.scanning = start_followup;
        if (status == ESP_OK) {
            UpdateScanResultsLocked(records.data(), count);
        } else {
            ESP_LOGW(kTag, "could not read scan results: %s", esp_err_to_name(status));
        }
        RebuildSnapshotLocked();
    }
    if (!start_followup) {
        return;
    }
    const esp_err_t followup_status = StartWifiScan(false);
    if (followup_status == ESP_OK) {
        ESP_LOGI(kTag, "quick scan complete: networks=%u; continuing full scan", count);
        return;
    }
    ESP_LOGW(kTag, "could not start full follow-up scan: %s", esp_err_to_name(followup_status));
    {
        ScopedLock lock(mutex_);
        snapshot_.scanning = false;
        RebuildSnapshotLocked();
    }
}

std::expected<void, device::WifiError> WifiBackend::ConfigureAndConnect(const SavedProfile& profile,
                                                                        bool full_channel_scan) {
    bool disconnect_existing = false;
    {
        ScopedLock lock(mutex_);
        scan_followup_pending_ = false;
        disconnect_existing = associated_ || snapshot_.connected;
        if (disconnect_existing) {
            ignore_next_disconnect_ = true;
            associated_ = false;
            snapshot_.connected = false;
        }
        snapshot_.scanning = false;
        snapshot_.connection_state = device::WifiConnectionState::kConnecting;
        RebuildSnapshotLocked();
    }
    (void)esp_wifi_scan_stop();
    if (disconnect_existing) {
        (void)esp_wifi_disconnect();
    }

    wifi_config_t config{};
    std::memcpy(config.sta.ssid, profile.ssid.data(), std::min(profile.ssid.size() - 1U, sizeof(config.sta.ssid)));
    std::memcpy(config.sta.password, profile.password.data(),
                std::min(profile.password.size() - 1U, sizeof(config.sta.password)));
    config.sta.scan_method = full_channel_scan ? WIFI_ALL_CHANNEL_SCAN : WIFI_FAST_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.channel = full_channel_scan ? 0U : profile.channel;
    config.sta.bssid_set = !full_channel_scan && profile.bssid_valid;
    if (config.sta.bssid_set) {
        std::memcpy(config.sta.bssid, profile.bssid.data(), profile.bssid.size());
    }
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    config.sta.failure_retry_cnt = 1U;

    ESP_LOGI(kTag,
             "connect start: ssid=%s band=%s channel=%u auth=%s targeted=%s "
             "bssid=%02x:%02x:%02x:%02x:%02x:%02x",
             profile.ssid.data(), BandForChannel(config.sta.channel) == device::WifiBand::k5Ghz ? "5GHz" : "2.4GHz",
             config.sta.channel, AuthModeName(profile.auth_mode), full_channel_scan ? "no" : "yes",
             profile.bssid[0], profile.bssid[1], profile.bssid[2], profile.bssid[3], profile.bssid[4],
             profile.bssid[5]);

    esp_err_t status = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (status == ESP_OK) {
        status = esp_wifi_connect();
    }
    if (status != ESP_OK) {
        ScopedLock lock(mutex_);
        snapshot_.connection_state = device::WifiConnectionState::kFailed;
        pending_profile_ = {};
        pending_profile_valid_ = false;
        RebuildSnapshotLocked();
        return std::unexpected(WifiErrorFor(status));
    }
    return {};
}

int32_t WifiBackend::FindSavedProfile(std::string_view ssid) const {
    for (uint32_t index = 0U; index < profile_count_; ++index) {
        if (SameSsid(profiles_[index].ssid, ssid)) {
            return static_cast<int32_t>(index);
        }
    }
    return -1;
}

int32_t WifiBackend::FindScanResult(std::string_view ssid) const {
    for (uint32_t index = 0U; index < scan_result_count_; ++index) {
        if (SameSsid(scan_results_[index].network.ssid, ssid)) {
            return static_cast<int32_t>(index);
        }
    }
    return -1;
}

bool WifiBackend::ApplyScanTarget(std::string_view ssid, SavedProfile& profile) const {
    const int32_t index = FindScanResult(ssid);
    if (index < 0) {
        return false;
    }
    const ScanResult& result = scan_results_[static_cast<uint32_t>(index)];
    if (result.network.channel == 0U) {
        return false;
    }
    profile.channel = result.network.channel;
    profile.bssid = result.bssid;
    profile.bssid_valid = true;
    profile.auth_mode = result.auth_mode;
    return true;
}

bool WifiBackend::LoadSettings() {
    nvs_handle_t handle{};
    esp_err_t status = nvs_open_from_partition(kNvsPartition, kNvsNamespace, NVS_READONLY, &handle);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "could not open saved networks: %s", esp_err_to_name(status));
        return false;
    }
    StoredState stored{};
    size_t size = sizeof(stored);
    status = nvs_get_blob(handle, kNvsStateKey, &stored, &size);
    nvs_close(handle);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (status != ESP_OK || size != sizeof(stored) || stored.magic != kStoredStateMagic ||
        stored.version != kStoredStateVersion || stored.profile_count > profiles_.size()) {
        ESP_LOGW(kTag, "saved network state is invalid or incompatible");
        return false;
    }
    profile_count_ = stored.profile_count;
    enabled_ = stored.enabled != 0U;
    active_profile_index_ =
        profile_count_ == 0U ? -1 : static_cast<int32_t>(std::min<uint32_t>(stored.last_profile, profile_count_ - 1U));
    for (uint32_t index = 0U; index < profile_count_; ++index) {
        const StoredProfile& source = stored.profiles[index];
        SavedProfile& destination = profiles_[index];
        std::memcpy(destination.ssid.data(), source.ssid, destination.ssid.size());
        destination.ssid.back() = '\0';
        std::memcpy(destination.password.data(), source.password, destination.password.size());
        destination.password.back() = '\0';
        std::memcpy(destination.bssid.data(), source.bssid, destination.bssid.size());
        destination.channel = source.channel;
        destination.secured = source.secured != 0U;
        destination.bssid_valid = source.bssid_valid != 0U;
    }
    return true;
}

bool WifiBackend::SaveSettings() const {
    StoredState stored{};
    {
        ScopedLock lock(mutex_);
        if (!lock.locked()) {
            return false;
        }
        stored.profile_count = static_cast<uint8_t>(profile_count_);
        stored.last_profile = active_profile_index_ >= 0 ? static_cast<uint8_t>(active_profile_index_) : 0U;
        stored.enabled = enabled_ ? 1U : 0U;
        for (uint32_t index = 0U; index < profile_count_; ++index) {
            const SavedProfile& source = profiles_[index];
            StoredProfile& destination = stored.profiles[index];
            std::memcpy(destination.ssid, source.ssid.data(), source.ssid.size());
            std::memcpy(destination.password, source.password.data(), source.password.size());
            std::memcpy(destination.bssid, source.bssid.data(), source.bssid.size());
            destination.channel = source.channel;
            destination.secured = source.secured ? 1U : 0U;
            destination.bssid_valid = source.bssid_valid ? 1U : 0U;
        }
    }

    nvs_handle_t handle{};
    esp_err_t status = nvs_open_from_partition(kNvsPartition, kNvsNamespace, NVS_READWRITE, &handle);
    if (status == ESP_OK) {
        status = nvs_set_blob(handle, kNvsStateKey, &stored, sizeof(stored));
    }
    if (status == ESP_OK) {
        status = nvs_commit(handle);
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "could not save Wi-Fi state: %s", esp_err_to_name(status));
    }
    return status == ESP_OK;
}

void WifiBackend::RebuildSnapshotLocked() {
    snapshot_.available = initialized_;
    snapshot_.enabled = enabled_;
    snapshot_.saved_network_count = profile_count_;
    snapshot_.available_network_count = 0U;
    for (auto& network : snapshot_.saved_networks) {
        network = {};
    }
    for (uint32_t index = 0U; index < profile_count_; ++index) {
        device::WifiNetwork& network = snapshot_.saved_networks[index];
        network.ssid = profiles_[index].ssid;
        network.channel = profiles_[index].channel;
        network.band = BandForChannel(network.channel);
        network.security = profiles_[index].secured ? device::WifiSecurity::kSecured : device::WifiSecurity::kOpen;
        network.saved = true;
        network.connected = snapshot_.connected && static_cast<int32_t>(index) == active_profile_index_;
        for (uint32_t scan_index = 0U; scan_index < scan_result_count_; ++scan_index) {
            const device::WifiNetwork& scanned = scan_results_[scan_index].network;
            if (SameSsid(scanned.ssid, network.ssid.data())) {
                network.rssi = scanned.rssi;
                if (!network.connected) {
                    network.channel = scanned.channel;
                    network.band = scanned.band;
                }
                break;
            }
        }
    }
    for (auto& network : snapshot_.available_networks) {
        network = {};
    }
    for (uint32_t scan_index = 0U;
         scan_index < scan_result_count_ && snapshot_.available_network_count < snapshot_.available_networks.size();
         ++scan_index) {
        const device::WifiNetwork& candidate = scan_results_[scan_index].network;
        if (FindSavedProfile(candidate.ssid.data()) >= 0) {
            continue;
        }
        snapshot_.available_networks[snapshot_.available_network_count++] = candidate;
    }
}

void WifiBackend::UpdateScanResultsLocked(const void* raw_records, uint16_t count) {
    const auto* records = static_cast<const wifi_ap_record_t*>(raw_records);
    scan_result_count_ = 0U;
    for (uint16_t index = 0U; index < count && scan_result_count_ < scan_results_.size(); ++index) {
        if (records[index].ssid[0] == '\0') {
            continue;
        }
        std::array<char, device::kWifiSsidCapacity + 1U> ssid{};
        CopyCString(ssid, records[index].ssid, sizeof(records[index].ssid));
        bool duplicate = false;
        for (uint32_t previous = 0U; previous < scan_result_count_; ++previous) {
            if (SameSsid(scan_results_[previous].network.ssid, ssid.data())) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        ScanResult& result = scan_results_[scan_result_count_++];
        device::WifiNetwork& network = result.network;
        network.ssid = ssid;
        network.rssi = records[index].rssi;
        network.channel = records[index].primary;
        network.band = BandForChannel(records[index].primary);
        network.security =
            records[index].authmode == WIFI_AUTH_OPEN ? device::WifiSecurity::kOpen : device::WifiSecurity::kSecured;
        std::memcpy(result.bssid.data(), records[index].bssid, result.bssid.size());
        result.auth_mode = static_cast<uint8_t>(records[index].authmode);
    }
}

}  // namespace micropixel::platform::metalio_claw4
