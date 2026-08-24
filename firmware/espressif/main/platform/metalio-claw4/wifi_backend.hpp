#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_WIFI_BACKEND_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_WIFI_BACKEND_HPP

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

#include "device/wifi.hpp"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace micropixel::platform::metalio_claw4 {

class WifiBackend final : public device::WifiBackend {
   public:
    WifiBackend() = default;

    [[nodiscard]] std::expected<void, device::WifiError> Initialize() override;
    [[nodiscard]] device::WifiSnapshot Snapshot() const override;
    void SetStateChangeSink(device::WifiStateChangeSink sink, void* context) override;
    [[nodiscard]] std::expected<void, device::WifiError> SetEnabled(bool enabled) override;
    [[nodiscard]] std::expected<void, device::WifiError> RequestScan() override;
    [[nodiscard]] std::expected<void, device::WifiError> ConnectSaved(std::string_view ssid) override;
    [[nodiscard]] std::expected<void, device::WifiError> Connect(std::string_view ssid,
                                                                 std::string_view password) override;
    [[nodiscard]] std::expected<void, device::WifiError> Disconnect() override;
    [[nodiscard]] std::expected<void, device::WifiError> Forget(std::string_view ssid) override;

   private:
    enum class ScanPurpose : uint8_t {
        kNone,
        kDiscovery,
        kUser,
    };

    struct SavedProfile final {
        std::array<char, device::kWifiSsidCapacity + 1U> ssid{};
        std::array<char, device::kWifiPasswordCapacity + 1U> password{};
        std::array<uint8_t, 6U> bssid{};
        uint8_t channel{};
        uint8_t auth_mode{0xffU};
        bool secured{true};
        bool bssid_valid{};
    };

    struct ScanResult final {
        device::WifiNetwork network{};
        std::array<uint8_t, 6U> bssid{};
        uint8_t auth_mode{0xffU};
    };

    static void WifiEventHandler(void* context, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void DiscoveryTimerHandler(void* context);
    void HandleWifiEvent(int32_t event_id, void* event_data);
    void HandleGotIp();
    void HandleScanDone();
    void HandleDiscoveryTimer();

    [[nodiscard]] std::expected<void, device::WifiError> ConfigureAndConnect(const SavedProfile& profile,
                                                                             bool full_channel_scan);
    [[nodiscard]] std::expected<void, device::WifiError> StartScan(ScanPurpose purpose);
    void StartDiscovery();
    void ScheduleDiscovery(bool advance_backoff);
    void ResetDiscoveryBackoff();
    [[nodiscard]] int32_t FindSavedProfile(std::string_view ssid) const;
    [[nodiscard]] int32_t FindScanResult(std::string_view ssid) const;
    [[nodiscard]] bool ApplyScanTarget(std::string_view ssid, SavedProfile& profile) const;
    [[nodiscard]] bool BuildDiscoveryChannelsLocked();
    [[nodiscard]] bool LoadSettings();
    [[nodiscard]] bool SaveSettings() const;
    void RebuildSnapshotLocked();
    void UpdateScanResultsLocked(const void* records, uint16_t count);
    void MergeDiscoveryScanResultsLocked(const void* records, uint16_t count);

    mutable SemaphoreHandle_t mutex_{};
    esp_timer_handle_t discovery_timer_{};
    esp_netif_t* station_netif_{};
    esp_event_handler_instance_t wifi_event_instance_{};
    esp_event_handler_instance_t ip_event_instance_{};
    std::array<SavedProfile, device::kMaxSavedWifiNetworks> profiles_{};
    SavedProfile pending_profile_{};
    std::array<ScanResult, device::kMaxVisibleWifiNetworks> scan_results_{};
    std::array<uint8_t, device::kMaxSavedWifiNetworks> discovery_channels_{};
    device::WifiSnapshot snapshot_{};
    uint32_t profile_count_{};
    uint32_t scan_result_count_{};
    uint32_t discovery_channel_count_{};
    uint32_t discovery_channel_index_{};
    int32_t active_profile_index_{-1};
    uint8_t reconnect_attempts_{};
    uint8_t discovery_backoff_index_{};
    int64_t last_user_scan_request_us_{};
    bool initialized_{};
    bool enabled_{true};
    bool associated_{};
    bool user_requested_disconnect_{};
    bool pending_profile_valid_{};
    bool ignore_next_disconnect_{};
    bool scan_after_disconnect_{};
    bool user_scan_after_discovery_{};
    bool discovery_connection_pending_{};
    bool connection_full_channel_scan_{};
    ScanPurpose scan_purpose_{ScanPurpose::kNone};
    device::WifiStateChangeSink state_change_sink_{};
    void* state_change_context_{};
};

}  // namespace micropixel::platform::metalio_claw4

#endif
