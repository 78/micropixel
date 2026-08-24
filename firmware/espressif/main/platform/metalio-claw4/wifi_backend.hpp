#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_WIFI_BACKEND_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_WIFI_BACKEND_HPP

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

#include "device/wifi.hpp"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace micropixel::platform::metalio_claw4 {

class WifiBackend final : public device::WifiBackend {
   public:
    WifiBackend() = default;

    [[nodiscard]] std::expected<void, device::WifiError> Initialize() override;
    [[nodiscard]] device::WifiSnapshot Snapshot() const override;
    [[nodiscard]] std::expected<void, device::WifiError> SetEnabled(bool enabled) override;
    [[nodiscard]] std::expected<void, device::WifiError> RequestScan() override;
    [[nodiscard]] std::expected<void, device::WifiError> ConnectSaved(std::string_view ssid) override;
    [[nodiscard]] std::expected<void, device::WifiError> Connect(std::string_view ssid,
                                                                 std::string_view password) override;
    [[nodiscard]] std::expected<void, device::WifiError> Disconnect() override;
    [[nodiscard]] std::expected<void, device::WifiError> Forget(std::string_view ssid) override;

   private:
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
    void HandleWifiEvent(int32_t event_id, void* event_data);
    void HandleGotIp();
    void HandleScanDone();

    [[nodiscard]] std::expected<void, device::WifiError> ConfigureAndConnect(const SavedProfile& profile,
                                                                             bool full_channel_scan);
    [[nodiscard]] int32_t FindSavedProfile(std::string_view ssid) const;
    [[nodiscard]] int32_t FindScanResult(std::string_view ssid) const;
    [[nodiscard]] bool ApplyScanTarget(std::string_view ssid, SavedProfile& profile) const;
    [[nodiscard]] bool LoadSettings();
    [[nodiscard]] bool SaveSettings() const;
    void RebuildSnapshotLocked();
    void UpdateScanResultsLocked(const void* records, uint16_t count);

    mutable SemaphoreHandle_t mutex_{};
    esp_netif_t* station_netif_{};
    esp_event_handler_instance_t wifi_event_instance_{};
    esp_event_handler_instance_t ip_event_instance_{};
    std::array<SavedProfile, device::kMaxSavedWifiNetworks> profiles_{};
    SavedProfile pending_profile_{};
    std::array<ScanResult, device::kMaxVisibleWifiNetworks> scan_results_{};
    device::WifiSnapshot snapshot_{};
    uint32_t profile_count_{};
    uint32_t scan_result_count_{};
    int32_t active_profile_index_{-1};
    uint8_t reconnect_attempts_{};
    bool initialized_{};
    bool enabled_{true};
    bool associated_{};
    bool user_requested_disconnect_{};
    bool pending_profile_valid_{};
    bool ignore_next_disconnect_{};
    bool scan_followup_pending_{};
};

}  // namespace micropixel::platform::metalio_claw4

#endif
