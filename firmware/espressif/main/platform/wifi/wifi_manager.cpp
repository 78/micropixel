#include "platform/wifi/wifi_manager.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs.h"
#include "work/background_executor.hpp"

namespace micropixel::platform::wifi {
namespace {

constexpr char kTag[] = "micropixel_wifi";
constexpr char kNvsPartition[] = "runtime_nvs";
constexpr char kNvsNamespace[] = "host_wifi";
constexpr char kNvsStateKey[] = "state";
constexpr uint32_t kStoredStateMagic = 0x57494649U;
constexpr uint16_t kStoredStateVersion = 1U;
constexpr uint8_t kReconnectAttemptLimit = 2U;
constexpr int64_t kUserScanDiscoveryHoldoffUs = 20LL * 1000LL * 1000LL;
constexpr std::array<int64_t, 4U> kDiscoveryBackoffUs = {
    60LL * 1000LL * 1000LL,
    2LL * 60LL * 1000LL * 1000LL,
    5LL * 60LL * 1000LL * 1000LL,
    15LL * 60LL * 1000LL * 1000LL,
};
constexpr size_t kScanRecordWorkspaceBytes =
    sizeof(wifi_ap_record_t) * static_cast<size_t>(device::kMaxVisibleWifiNetworks);

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

esp_err_t StartWifiScan(bool passive, uint8_t channel) {
    wifi_scan_config_t config{};
    config.show_hidden = false;
    config.scan_type = passive ? WIFI_SCAN_TYPE_PASSIVE : WIFI_SCAN_TYPE_ACTIVE;
    config.channel = channel;
    if (passive) {
        config.scan_time.passive = WIFI_PASSIVE_SCAN_DEFAULT_TIME;
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

}  // namespace

void WifiManager::HeapCapsDeleter::operator()(std::byte* value) const { heap_caps_free(value); }

void WifiManager::BindBackgroundExecutor(work::BackgroundExecutor& executor) { background_executor_ = &executor; }

std::expected<void, device::WifiError> WifiManager::Initialize() {
    if (initialized_) {
        return {};
    }
    mutex_ = xSemaphoreCreateMutex();
    persistence_mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr || persistence_mutex_ == nullptr) {
        return std::unexpected(device::WifiError::kOperationFailed);
    }
    auto* scan_record_workspace =
        static_cast<std::byte*>(heap_caps_calloc(1U, kScanRecordWorkspaceBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scan_record_workspace == nullptr) {
        scan_record_workspace =
            static_cast<std::byte*>(heap_caps_calloc(1U, kScanRecordWorkspaceBytes, MALLOC_CAP_8BIT));
    }
    if (scan_record_workspace == nullptr) {
        return std::unexpected(device::WifiError::kOperationFailed);
    }
    scan_record_workspace_.reset(scan_record_workspace);
    const esp_timer_create_args_t discovery_timer_args{
        .callback = DiscoveryTimerHandler,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_discovery",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&discovery_timer_args, &discovery_timer_) != ESP_OK) {
        return std::unexpected(device::WifiError::kOperationFailed);
    }
    (void)LoadSettings();

    const esp_err_t radio_status = radio_.Initialize();
    if (radio_status != ESP_OK) {
        ESP_LOGW(kTag, "%s radio initialization failed: %s", radio_.Name(), esp_err_to_name(radio_status));
        return std::unexpected(WifiErrorFor(radio_status));
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
    // MicroPixel owns Wi-Fi persistence in runtime_nvs and explicitly uses
    // WIFI_STORAGE_RAM below. Disable the driver's separate default-NVS path
    // so native and hosted radios share one authoritative settings store.
    config.nvs_enable = false;
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
        status = radio_.OnStationStarted();
        if (status != ESP_OK) {
            ESP_LOGW(kTag, "%s radio post-start configuration failed: %s", radio_.Name(), esp_err_to_name(status));
            return std::unexpected(WifiErrorFor(status));
        }
    }
    ESP_LOGI(kTag, "Wi-Fi manager ready: radio=%s enabled=%s saved=%lu", radio_.Name(), enabled_ ? "yes" : "no",
             static_cast<unsigned long>(profile_count_));
    return {};
}

device::WifiSnapshot WifiManager::Snapshot() const {
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

void WifiManager::SetStateChangeSink(device::WifiStateChangeSink sink, void* context) {
    if (mutex_ == nullptr) {
        state_change_sink_ = sink;
        state_change_context_ = context;
    } else {
        ScopedLock lock(mutex_);
        state_change_sink_ = sink;
        state_change_context_ = context;
    }
    if (sink != nullptr) {
        sink(context);
    }
}

std::expected<void, device::WifiError> WifiManager::SetEnabled(bool enabled) {
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
        scan_after_disconnect_ = false;
        user_scan_after_discovery_ = false;
        discovery_connection_pending_ = false;
        connection_full_channel_scan_ = false;
        scan_purpose_ = ScanPurpose::kNone;
        discovery_channel_count_ = 0U;
        discovery_channel_index_ = 0U;
        discovery_backoff_index_ = 0U;
        last_user_scan_request_us_ = 0;
        snapshot_.enabled = enabled;
        snapshot_.connected = false;
        snapshot_.scanning = false;
        snapshot_.connection_state = device::WifiConnectionState::kDisconnected;
        RebuildSnapshotLocked();
    }

    if (discovery_timer_ != nullptr) {
        (void)esp_timer_stop(discovery_timer_);
    }

    esp_err_t status = enabled ? esp_wifi_start() : esp_wifi_stop();
#if CONFIG_ESP_HOSTED_CP_TARGET_ESP32C5
    if (status == ESP_OK && enabled) {
        status = esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
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
    QueueSaveSettings();
    return {};
}

std::expected<void, device::WifiError> WifiManager::RequestScan() {
    bool disconnect_connecting = false;
    {
        ScopedLock lock(mutex_);
        if (!initialized_ || !enabled_) {
            return std::unexpected(device::WifiError::kUnavailable);
        }
        last_user_scan_request_us_ = esp_timer_get_time();
        if (snapshot_.scanning) {
            if (scan_purpose_ == ScanPurpose::kDiscovery) {
                user_scan_after_discovery_ = true;
            }
            return {};
        }
        if (snapshot_.connection_state == device::WifiConnectionState::kConnecting && !snapshot_.connected) {
            scan_after_disconnect_ = true;
            scan_purpose_ = ScanPurpose::kUser;
            disconnect_connecting = true;
        }
    }
    if (!disconnect_connecting) {
        return StartScan(ScanPurpose::kUser);
    }
    const esp_err_t disconnect_status = esp_wifi_disconnect();
    if (disconnect_status == ESP_ERR_WIFI_NOT_CONNECT) {
        {
            ScopedLock lock(mutex_);
            scan_after_disconnect_ = false;
        }
        return StartScan(ScanPurpose::kUser);
    }
    if (disconnect_status != ESP_OK) {
        ScopedLock lock(mutex_);
        scan_after_disconnect_ = false;
        scan_purpose_ = ScanPurpose::kNone;
        return std::unexpected(WifiErrorFor(disconnect_status));
    }
    return {};
}

std::expected<void, device::WifiError> WifiManager::StartScan(ScanPurpose purpose) {
    uint8_t channel = 0U;
    {
        ScopedLock lock(mutex_);
        if (!initialized_ || !enabled_) {
            return std::unexpected(device::WifiError::kUnavailable);
        }
        if (snapshot_.scanning || snapshot_.connection_state == device::WifiConnectionState::kConnecting) {
            return std::unexpected(device::WifiError::kBusy);
        }
        if (purpose == ScanPurpose::kDiscovery) {
            if (!BuildDiscoveryChannelsLocked()) {
                return std::unexpected(device::WifiError::kUnavailable);
            }
            channel = discovery_channels_[0U];
        } else {
            discovery_channel_count_ = 0U;
            discovery_channel_index_ = 0U;
        }
        snapshot_.scanning = true;
        scan_purpose_ = purpose;
        RebuildSnapshotLocked();
    }
    const bool passive = purpose == ScanPurpose::kDiscovery;
    if (passive) {
        ESP_LOGI(kTag, "passive saved-network scan start: channel=%u", channel);
    } else {
        ESP_LOGI(kTag, "active full-channel Wi-Fi scan start");
    }
    const esp_err_t status = StartWifiScan(passive, channel);
    if (status != ESP_OK) {
        ScopedLock lock(mutex_);
        snapshot_.scanning = false;
        scan_purpose_ = ScanPurpose::kNone;
        discovery_channel_count_ = 0U;
        discovery_channel_index_ = 0U;
        RebuildSnapshotLocked();
        return std::unexpected(WifiErrorFor(status));
    }
    return {};
}

void WifiManager::StartDiscovery() {
    {
        ScopedLock lock(mutex_);
        if (!initialized_ || !enabled_ || profile_count_ == 0U || snapshot_.connected || user_requested_disconnect_) {
            return;
        }
        const int64_t now_us = esp_timer_get_time();
        if (last_user_scan_request_us_ != 0 && now_us - last_user_scan_request_us_ < kUserScanDiscoveryHoldoffUs) {
            if (discovery_timer_ != nullptr) {
                (void)esp_timer_stop(discovery_timer_);
                (void)esp_timer_start_once(discovery_timer_, kUserScanDiscoveryHoldoffUs);
            }
            return;
        }
    }
    const auto result = StartScan(ScanPurpose::kDiscovery);
    if (!result) {
        ScheduleDiscovery(true);
    }
}

void WifiManager::ScheduleDiscovery(bool advance_backoff) {
    int64_t delay_us = kDiscoveryBackoffUs.back();
    {
        ScopedLock lock(mutex_);
        if (!initialized_ || !enabled_ || snapshot_.connected || user_requested_disconnect_ ||
            discovery_timer_ == nullptr) {
            return;
        }
        const uint32_t index = std::min<uint32_t>(discovery_backoff_index_, kDiscoveryBackoffUs.size() - 1U);
        delay_us = kDiscoveryBackoffUs[index];
        if (advance_backoff && discovery_backoff_index_ + 1U < kDiscoveryBackoffUs.size()) {
            ++discovery_backoff_index_;
        }
    }
    (void)esp_timer_stop(discovery_timer_);
    if (esp_timer_start_once(discovery_timer_, delay_us) != ESP_OK) {
        ESP_LOGW(kTag, "could not schedule Wi-Fi discovery retry");
    }
}

void WifiManager::ResetDiscoveryBackoff() {
    {
        ScopedLock lock(mutex_);
        discovery_backoff_index_ = 0U;
        discovery_connection_pending_ = false;
    }
    if (discovery_timer_ != nullptr) {
        (void)esp_timer_stop(discovery_timer_);
    }
}

void WifiManager::DiscoveryTimerHandler(void* context) {
    auto* manager = static_cast<WifiManager*>(context);
    if (manager != nullptr) {
        manager->HandleDiscoveryTimer();
    }
}

void WifiManager::HandleDiscoveryTimer() { StartDiscovery(); }

std::expected<void, device::WifiError> WifiManager::ConnectSaved(std::string_view ssid) {
    SavedProfile profile{};
    int32_t index = -1;
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
        active_profile_index_ = index;
        pending_profile_valid_ = false;
        user_requested_disconnect_ = false;
        reconnect_attempts_ = 0U;
        discovery_connection_pending_ = false;
    }
    ResetDiscoveryBackoff();
    // An explicit Saved Network -> Connect action must tolerate an AP moving
    // between channels or bands. Keep targeted connections for automatic
    // discovery, but let the driver search all supported channels here.
    return ConfigureAndConnect(profile, true);
}

std::expected<void, device::WifiError> WifiManager::Connect(std::string_view ssid, std::string_view password) {
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
        discovery_connection_pending_ = false;
    }
    ResetDiscoveryBackoff();
    return ConfigureAndConnect(profile, !have_target);
}

std::expected<void, device::WifiError> WifiManager::Disconnect() {
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
        scan_after_disconnect_ = false;
        user_scan_after_discovery_ = false;
        discovery_connection_pending_ = false;
        connection_full_channel_scan_ = false;
        scan_purpose_ = ScanPurpose::kNone;
        discovery_channel_count_ = 0U;
        discovery_channel_index_ = 0U;
        snapshot_.connected = false;
        snapshot_.connection_state = device::WifiConnectionState::kDisconnected;
        RebuildSnapshotLocked();
    }
    if (discovery_timer_ != nullptr) {
        (void)esp_timer_stop(discovery_timer_);
    }
    const esp_err_t status = esp_wifi_disconnect();
    if (status != ESP_OK && status != ESP_ERR_WIFI_NOT_CONNECT) {
        return std::unexpected(WifiErrorFor(status));
    }
    return {};
}

std::expected<void, device::WifiError> WifiManager::Forget(std::string_view ssid) {
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
            connection_full_channel_scan_ = false;
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

void WifiManager::WifiEventHandler(void* context, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* manager = static_cast<WifiManager*>(context);
    if (manager == nullptr) {
        return;
    }
    if (event_base == WIFI_EVENT) {
        manager->HandleWifiEvent(event_id, event_data);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        manager->HandleGotIp();
    }
}

void WifiManager::HandleWifiEvent(int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_SCAN_DONE) {
        HandleScanDone();
        return;
    }
    if (event_id == WIFI_EVENT_STA_START) {
        bool should_discover = false;
        {
            ScopedLock lock(mutex_);
            should_discover = enabled_ && profile_count_ > 0U;
            user_requested_disconnect_ = false;
            reconnect_attempts_ = 0U;
            discovery_backoff_index_ = 0U;
        }
        if (should_discover) {
            StartDiscovery();
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
        QueueSaveSettings();
        return;
    }
    if (event_id != WIFI_EVENT_STA_DISCONNECTED) {
        return;
    }

    const auto* disconnected = static_cast<const wifi_event_sta_disconnected_t*>(event_data);
    const uint8_t disconnect_reason =
        disconnected == nullptr ? static_cast<uint8_t>(WIFI_REASON_UNSPECIFIED) : disconnected->reason;
    if (disconnected != nullptr) {
        ESP_LOGW(kTag, "station disconnected: reason=%u ssid=%.*s rssi=%d bssid=%02x:%02x:%02x:%02x:%02x:%02x",
                 disconnect_reason, static_cast<int>(disconnected->ssid_len),
                 reinterpret_cast<const char*>(disconnected->ssid), disconnected->rssi, disconnected->bssid[0],
                 disconnected->bssid[1], disconnected->bssid[2], disconnected->bssid[3], disconnected->bssid[4],
                 disconnected->bssid[5]);
    } else {
        ESP_LOGW(kTag, "station disconnected without event data: reason=%u", disconnect_reason);
    }
    bool should_retry = false;
    bool start_user_scan = false;
    bool start_discovery = false;
    bool schedule_discovery = false;
    bool user_connection_failed = false;
    bool retry_new_connection = false;
    bool retry_full_channel_scan = false;
    SavedProfile retry_profile{};
    uint8_t retry_attempt = 0U;
    {
        ScopedLock lock(mutex_);
        associated_ = false;
        snapshot_.connected = false;
        if (scan_after_disconnect_) {
            scan_after_disconnect_ = false;
            snapshot_.connection_state = device::WifiConnectionState::kDisconnected;
            RebuildSnapshotLocked();
            start_user_scan = true;
        } else if (ignore_next_disconnect_) {
            ignore_next_disconnect_ = false;
            RebuildSnapshotLocked();
            return;
        } else if (user_requested_disconnect_) {
            snapshot_.connection_state = device::WifiConnectionState::kDisconnected;
            RebuildSnapshotLocked();
            return;
        } else {
            const bool new_connection_pending = pending_profile_valid_;
            should_retry = enabled_ && reconnect_attempts_ < kReconnectAttemptLimit &&
                           (new_connection_pending || active_profile_index_ >= 0);
            if (should_retry) {
                ++reconnect_attempts_;
                retry_attempt = reconnect_attempts_;
                retry_new_connection = new_connection_pending;
                retry_profile =
                    new_connection_pending ? pending_profile_ : profiles_[static_cast<uint32_t>(active_profile_index_)];
                retry_full_channel_scan =
                    connection_full_channel_scan_ || retry_profile.channel == 0U || !retry_profile.bssid_valid;
                snapshot_.connection_state = device::WifiConnectionState::kConnecting;
            } else if (new_connection_pending) {
                user_connection_failed = true;
                pending_profile_ = {};
                pending_profile_valid_ = false;
                snapshot_.connection_state = ConnectionStateForDisconnectReason(disconnect_reason);
                discovery_connection_pending_ = false;
                connection_full_channel_scan_ = false;
                schedule_discovery = profile_count_ > 0U;
            } else {
                snapshot_.connection_state = device::WifiConnectionState::kFailed;
                connection_full_channel_scan_ = false;
                if (discovery_connection_pending_) {
                    discovery_connection_pending_ = false;
                    schedule_discovery = profile_count_ > 0U;
                } else {
                    start_discovery = profile_count_ > 0U;
                }
            }
            RebuildSnapshotLocked();
        }
    }
    if (start_user_scan) {
        if (const auto result = StartScan(ScanPurpose::kUser); !result) {
            ESP_LOGW(kTag, "deferred user Wi-Fi scan failed: error=%u", static_cast<unsigned>(result.error()));
        }
        return;
    }
    if (user_connection_failed) {
        ESP_LOGW(kTag, "new Wi-Fi connection failed: reason=%u (%s)", disconnect_reason,
                 disconnect_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ? "AP-reported 4-way handshake timeout"
                 : disconnect_reason == WIFI_REASON_HANDSHAKE_TIMEOUT    ? "local handshake timeout"
                                                                         : "see wifi_err_reason_t");
    }
    if (should_retry) {
        const auto result = ConfigureAndConnect(retry_profile, retry_full_channel_scan);
        ESP_LOGI(kTag, "%s connection retry attempt %u: %s", retry_new_connection ? "new Wi-Fi" : "saved Wi-Fi",
                 retry_attempt, result ? "started" : "failed");
        if (!result) {
            StartDiscovery();
        }
    } else if (start_discovery) {
        StartDiscovery();
    } else if (schedule_discovery) {
        ScheduleDiscovery(true);
    }
}

void WifiManager::HandleGotIp() {
    {
        ScopedLock lock(mutex_);
        if (!associated_) {
            return;
        }
        snapshot_.connected = true;
        snapshot_.connection_state = device::WifiConnectionState::kConnected;
        reconnect_attempts_ = 0U;
        connection_full_channel_scan_ = false;
        RebuildSnapshotLocked();
    }
    ResetDiscoveryBackoff();
}

void WifiManager::HandleScanDone() {
    auto* records = reinterpret_cast<wifi_ap_record_t*>(scan_record_workspace_.get());
    uint16_t count = static_cast<uint16_t>(device::kMaxVisibleWifiNetworks);
    const esp_err_t status = records == nullptr ? ESP_ERR_NO_MEM : esp_wifi_scan_get_ap_records(&count, records);
    SavedProfile discovery_profile{};
    bool connect_discovery_candidate = false;
    bool start_next_discovery_channel = false;
    bool start_user_scan = false;
    bool schedule_discovery = false;
    bool advance_discovery_backoff = true;
    uint8_t next_discovery_channel = 0U;
    {
        ScopedLock lock(mutex_);
        if (scan_purpose_ == ScanPurpose::kDiscovery) {
            if (status == ESP_OK) {
                MergeDiscoveryScanResultsLocked(records, count);
            } else {
                ESP_LOGW(kTag, "could not read passive discovery results: %s", esp_err_to_name(status));
            }

            if (user_scan_after_discovery_) {
                user_scan_after_discovery_ = false;
                start_user_scan = true;
            }

            int32_t best_profile_index = -1;
            int8_t best_rssi = -128;
            uint16_t best_record_index = 0U;
            for (uint16_t record_index = 0U; !start_user_scan && record_index < count; ++record_index) {
                std::array<char, device::kWifiSsidCapacity + 1U> ssid{};
                CopyCString(ssid, records[record_index].ssid, sizeof(records[record_index].ssid));
                const int32_t profile_index = FindSavedProfile(ssid.data());
                if (profile_index >= 0 && (best_profile_index < 0 || records[record_index].rssi > best_rssi)) {
                    best_profile_index = profile_index;
                    best_record_index = record_index;
                    best_rssi = records[record_index].rssi;
                }
            }

            if (start_user_scan) {
                snapshot_.scanning = false;
                scan_purpose_ = ScanPurpose::kNone;
                discovery_channel_count_ = 0U;
                discovery_channel_index_ = 0U;
                RebuildSnapshotLocked();
            } else if (best_profile_index >= 0) {
                active_profile_index_ = best_profile_index;
                SavedProfile& saved_profile = profiles_[static_cast<uint32_t>(best_profile_index)];
                const wifi_ap_record_t& record = records[best_record_index];
                saved_profile.channel = record.primary;
                std::memcpy(saved_profile.bssid.data(), record.bssid, saved_profile.bssid.size());
                saved_profile.bssid_valid = true;
                saved_profile.auth_mode = static_cast<uint8_t>(record.authmode);
                discovery_profile = saved_profile;
                discovery_connection_pending_ = true;
                reconnect_attempts_ = 0U;
                connect_discovery_candidate = true;
                snapshot_.scanning = false;
                scan_purpose_ = ScanPurpose::kNone;
                discovery_channel_count_ = 0U;
                discovery_channel_index_ = 0U;
                RebuildSnapshotLocked();
            } else if (discovery_channel_index_ + 1U < discovery_channel_count_) {
                ++discovery_channel_index_;
                next_discovery_channel = discovery_channels_[discovery_channel_index_];
                start_next_discovery_channel = true;
            } else {
                schedule_discovery = profile_count_ > 0U;
                snapshot_.scanning = false;
                scan_purpose_ = ScanPurpose::kNone;
                discovery_channel_count_ = 0U;
                discovery_channel_index_ = 0U;
                RebuildSnapshotLocked();
            }
        } else if (scan_purpose_ == ScanPurpose::kUser) {
            snapshot_.scanning = false;
            if (status == ESP_OK) {
                UpdateScanResultsLocked(records, count);
            } else {
                ESP_LOGW(kTag, "could not read active scan results: %s", esp_err_to_name(status));
            }
            if (!snapshot_.connected && !user_requested_disconnect_) {
                schedule_discovery = profile_count_ > 0U;
                advance_discovery_backoff = false;
            }
            scan_purpose_ = ScanPurpose::kNone;
            RebuildSnapshotLocked();
        } else {
            snapshot_.scanning = false;
            RebuildSnapshotLocked();
        }
    }

    if (start_next_discovery_channel) {
        ESP_LOGI(kTag, "passive saved-network scan continue: channel=%u", next_discovery_channel);
        if (const esp_err_t next_status = StartWifiScan(true, next_discovery_channel); next_status != ESP_OK) {
            {
                ScopedLock lock(mutex_);
                snapshot_.scanning = false;
                scan_purpose_ = ScanPurpose::kNone;
                discovery_channel_count_ = 0U;
                discovery_channel_index_ = 0U;
                RebuildSnapshotLocked();
            }
            ESP_LOGW(kTag, "could not continue passive Wi-Fi discovery: %s", esp_err_to_name(next_status));
            ScheduleDiscovery(true);
        }
    } else if (start_user_scan) {
        if (const auto result = StartScan(ScanPurpose::kUser); !result) {
            ESP_LOGW(kTag, "deferred active Wi-Fi scan failed: error=%u", static_cast<unsigned>(result.error()));
            ScheduleDiscovery(false);
        }
    } else if (connect_discovery_candidate) {
        const auto result = ConfigureAndConnect(discovery_profile, false);
        if (!result) {
            {
                ScopedLock lock(mutex_);
                discovery_connection_pending_ = false;
            }
            ScheduleDiscovery(true);
        }
    } else if (schedule_discovery) {
        ScheduleDiscovery(advance_discovery_backoff);
    }
}

std::expected<void, device::WifiError> WifiManager::ConfigureAndConnect(const SavedProfile& profile,
                                                                        bool full_channel_scan) {
    bool disconnect_existing = false;
    {
        ScopedLock lock(mutex_);
        scan_purpose_ = ScanPurpose::kNone;
        connection_full_channel_scan_ = full_channel_scan;
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
    // A BSSID identifies one radio, not the saved network. Let the station
    // select any AP advertising the requested SSID so roaming, mesh nodes and
    // randomized BSSIDs do not turn a fresh scan result into a stale target.
    config.sta.bssid_set = false;
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    ESP_LOGI(kTag,
             "connect start: ssid=%s band=%s channel=%u auth=%s scan=%s "
             "candidate_bssid=%02x:%02x:%02x:%02x:%02x:%02x",
             profile.ssid.data(), BandForChannel(config.sta.channel) == device::WifiBand::k5Ghz ? "5GHz" : "2.4GHz",
             config.sta.channel, AuthModeName(profile.auth_mode), full_channel_scan ? "full" : "channel",
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
        connection_full_channel_scan_ = false;
        RebuildSnapshotLocked();
        return std::unexpected(WifiErrorFor(status));
    }
    return {};
}

int32_t WifiManager::FindSavedProfile(std::string_view ssid) const {
    for (uint32_t index = 0U; index < profile_count_; ++index) {
        if (SameSsid(profiles_[index].ssid, ssid)) {
            return static_cast<int32_t>(index);
        }
    }
    return -1;
}

int32_t WifiManager::FindScanResult(std::string_view ssid) const {
    for (uint32_t index = 0U; index < scan_result_count_; ++index) {
        if (SameSsid(scan_results_[index].network.ssid, ssid)) {
            return static_cast<int32_t>(index);
        }
    }
    return -1;
}

bool WifiManager::ApplyScanTarget(std::string_view ssid, SavedProfile& profile) const {
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

bool WifiManager::BuildDiscoveryChannelsLocked() {
    discovery_channels_.fill(0U);
    discovery_channel_count_ = 0U;
    discovery_channel_index_ = 0U;

    const auto append_channel = [this](uint8_t channel) {
        if (channel == 0U) {
            return;
        }
        for (uint32_t index = 0U; index < discovery_channel_count_; ++index) {
            if (discovery_channels_[index] == channel) {
                return;
            }
        }
        if (discovery_channel_count_ < discovery_channels_.size()) {
            discovery_channels_[discovery_channel_count_++] = channel;
        }
    };

    if (active_profile_index_ >= 0 && active_profile_index_ < static_cast<int32_t>(profile_count_)) {
        append_channel(profiles_[static_cast<uint32_t>(active_profile_index_)].channel);
    }
    for (uint32_t index = 0U; index < profile_count_; ++index) {
        append_channel(profiles_[index].channel);
    }
    return discovery_channel_count_ != 0U;
}

bool WifiManager::LoadSettings() {
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

bool WifiManager::SaveSettings() const {
    ScopedLock persistence_lock(persistence_mutex_);
    if (!persistence_lock.locked()) {
        return false;
    }
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

void WifiManager::QueueSaveSettings() {
    save_request_generation_.fetch_add(1U, std::memory_order_release);
    bool expected = false;
    if (!save_job_scheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    if (background_executor_ == nullptr || !background_executor_->Submit(SaveSettingsEntry, this)) {
        save_job_scheduled_.store(false, std::memory_order_release);
        ESP_LOGW(kTag, "could not queue Wi-Fi settings persistence");
    }
}

void WifiManager::SaveSettingsEntry(void* context) {
    auto* manager = static_cast<WifiManager*>(context);
    if (manager != nullptr) {
        manager->ProcessPendingSettingsSaves();
    }
}

void WifiManager::ProcessPendingSettingsSaves() {
    for (;;) {
        const uint32_t target_generation = save_request_generation_.load(std::memory_order_acquire);
        (void)SaveSettings();
        if (save_request_generation_.load(std::memory_order_acquire) != target_generation) {
            continue;
        }

        save_job_scheduled_.store(false, std::memory_order_release);
        if (save_request_generation_.load(std::memory_order_acquire) == target_generation) {
            return;
        }

        bool expected = false;
        if (!save_job_scheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
    }
}

void WifiManager::RebuildSnapshotLocked() {
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
        device::WifiNetwork candidate = scan_results_[scan_index].network;
        const int32_t saved_index = FindSavedProfile(candidate.ssid.data());
        if (saved_index >= 0) {
            candidate.saved = true;
            candidate.connected = snapshot_.connected && saved_index == active_profile_index_;
        }
        snapshot_.available_networks[snapshot_.available_network_count++] = candidate;
    }
    if (state_change_sink_ != nullptr) {
        state_change_sink_(state_change_context_);
    }
}

void WifiManager::UpdateScanResultsLocked(const void* raw_records, uint16_t count) {
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

void WifiManager::MergeDiscoveryScanResultsLocked(const void* raw_records, uint16_t count) {
    const auto* records = static_cast<const wifi_ap_record_t*>(raw_records);
    for (uint16_t record_index = 0U; record_index < count; ++record_index) {
        if (records[record_index].ssid[0] == '\0') {
            continue;
        }
        std::array<char, device::kWifiSsidCapacity + 1U> ssid{};
        CopyCString(ssid, records[record_index].ssid, sizeof(records[record_index].ssid));
        if (FindSavedProfile(ssid.data()) < 0) {
            continue;
        }

        int32_t result_index = FindScanResult(ssid.data());
        if (result_index < 0) {
            if (scan_result_count_ >= scan_results_.size()) {
                continue;
            }
            result_index = static_cast<int32_t>(scan_result_count_++);
        }
        ScanResult& result = scan_results_[static_cast<uint32_t>(result_index)];
        result.network.ssid = ssid;
        result.network.rssi = records[record_index].rssi;
        result.network.channel = records[record_index].primary;
        result.network.band = BandForChannel(records[record_index].primary);
        result.network.security = records[record_index].authmode == WIFI_AUTH_OPEN ? device::WifiSecurity::kOpen
                                                                                   : device::WifiSecurity::kSecured;
        std::memcpy(result.bssid.data(), records[record_index].bssid, result.bssid.size());
        result.auth_mode = static_cast<uint8_t>(records[record_index].authmode);
    }
}

}  // namespace micropixel::platform::wifi
