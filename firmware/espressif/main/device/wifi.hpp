#ifndef MICROPIXEL_DEVICE_WIFI_HPP
#define MICROPIXEL_DEVICE_WIFI_HPP

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

namespace micropixel::device {

constexpr uint32_t kWifiSsidCapacity = 32U;
constexpr uint32_t kWifiPasswordCapacity = 64U;
constexpr uint32_t kMaxSavedWifiNetworks = 8U;
constexpr uint32_t kMaxVisibleWifiNetworks = 16U;

enum class WifiBand : uint8_t {
    kUnknown,
    k2_4Ghz,
    k5Ghz,
};

enum class WifiSecurity : uint8_t {
    kOpen,
    kSecured,
};

enum class WifiConnectionState : uint8_t {
    kDisconnected,
    kConnecting,
    kConnected,
    kAuthenticationFailed,
    kAuthenticationTimedOut,
    kHandshakeTimedOut,
    kNetworkNotFound,
    kFailed,
};

struct WifiNetwork final {
    std::array<char, kWifiSsidCapacity + 1U> ssid{};
    int8_t rssi{};
    uint8_t channel{};
    WifiBand band{WifiBand::kUnknown};
    WifiSecurity security{WifiSecurity::kSecured};
    bool saved{};
    bool connected{};
};

struct WifiSnapshot final {
    std::array<WifiNetwork, kMaxSavedWifiNetworks> saved_networks{};
    std::array<WifiNetwork, kMaxVisibleWifiNetworks> available_networks{};
    uint32_t saved_network_count{};
    uint32_t available_network_count{};
    bool available{};
    bool enabled{};
    bool connected{};
    bool scanning{};
    WifiConnectionState connection_state{WifiConnectionState::kDisconnected};
};

enum class WifiError : uint8_t {
    kUnavailable,
    kInvalidArgument,
    kBusy,
    kStorage,
    kOperationFailed,
};

class WifiBackend {
   public:
    virtual ~WifiBackend() = default;
    WifiBackend(const WifiBackend&) = delete;
    WifiBackend& operator=(const WifiBackend&) = delete;

    [[nodiscard]] virtual std::expected<void, WifiError> Initialize() = 0;
    [[nodiscard]] virtual WifiSnapshot Snapshot() const = 0;
    [[nodiscard]] virtual std::expected<void, WifiError> SetEnabled(bool enabled) = 0;
    [[nodiscard]] virtual std::expected<void, WifiError> RequestScan() = 0;
    [[nodiscard]] virtual std::expected<void, WifiError> ConnectSaved(std::string_view ssid) = 0;
    [[nodiscard]] virtual std::expected<void, WifiError> Connect(std::string_view ssid, std::string_view password) = 0;
    [[nodiscard]] virtual std::expected<void, WifiError> Disconnect() = 0;
    [[nodiscard]] virtual std::expected<void, WifiError> Forget(std::string_view ssid) = 0;

   protected:
    WifiBackend() = default;
};

}  // namespace micropixel::device

#endif
