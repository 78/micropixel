#pragma once

#include <mutex>

#include "device/contracts/wifi.hpp"
#include "work/background_executor.hpp"

namespace micropixel::host::network {

// Host-facing Wi-Fi: commands and driver/RPC snapshots run on a dedicated
// executor. Its lifetime must outlast this adapter; Shutdown drains it here.
class AsyncWifi final : public device::Wifi {
   public:
    AsyncWifi(device::Wifi& driver, work::BackgroundExecutor& worker);
    ~AsyncWifi() override;
    std::expected<void, device::WifiError> Initialize() override;
    device::WifiSnapshot Snapshot() const override;
    void SetStateChangeSink(device::WifiStateChangeSink sink, void* context) override;
    void Poll() override;
    std::expected<void, device::WifiError> SetEnabled(bool enabled) override;
    std::expected<void, device::WifiError> RequestScan() override;
    std::expected<void, device::WifiError> ConnectSaved(std::string_view ssid) override;
    std::expected<void, device::WifiError> Connect(std::string_view ssid, std::string_view password) override;
    std::expected<void, device::WifiError> Disconnect() override;
    std::expected<void, device::WifiError> Forget(std::string_view ssid) override;

   private:
    enum class Operation { kNone, kInitialize, kEnable, kScan, kConnectSaved, kConnect, kDisconnect, kForget };
    struct Command {
        Operation operation{};
        bool enabled{};
        std::array<char, device::kWifiSsidCapacity + 1> ssid{};
        std::array<char, device::kWifiPasswordCapacity + 1> password{};
    };
    std::expected<void, device::WifiError> Submit(Operation operation, bool enabled = false, std::string_view ssid = {},
                                                  std::string_view password = {});
    static void Changed(void* context);
    static void Run(void* context);
    void Drain();
    bool ScheduleLocked();
    void NotifyLocked();
    device::Wifi& driver_;
    work::BackgroundExecutor& worker_;
    mutable std::mutex mutex_;
    device::WifiSnapshot snapshot_{};
    device::WifiSnapshot workspace_{};  // Worker-owned, never materialized on its stack.
    Command command_{};
    bool scheduled_{};
    bool refresh_needed_{};
    bool stopping_{};
    device::WifiStateChangeSink sink_{};
    void* sink_context_{};
};

}  // namespace micropixel::host::network
