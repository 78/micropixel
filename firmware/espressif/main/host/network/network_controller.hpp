#pragma once

#include <mutex>

#include "host/network/network.hpp"

namespace micropixel::host::network {

class NetworkController final : public Network {
   public:
    NetworkController(device::Wifi& wifi, device::Cellular& cellular, device::NetworkRouteSource& routes);
    ~NetworkController() override;
    void CopySnapshot(NetworkSnapshot& destination) const override;
    device::Wifi& WifiControl() override { return wifi_control_; }
    device::Cellular& CellularControl() override { return cellular_control_; }
    void SetStateChangeSink(NetworkStateChangeSink sink, void* context) override;
    bool TryBeginFirmwareUpdate() override;
    void EndFirmwareUpdate() override;
    // Called by a process-lifetime scheduler, regardless of which page/app is visible.
    // Only submits bounded work; AT commands run on the existing background executor.
    void Poll();

   private:
    class WifiControlView final : public device::Wifi {
       public:
        explicit WifiControlView(NetworkController& owner) : owner_(owner) {}
        std::expected<void, device::WifiError> Initialize() override;
        device::WifiSnapshot Snapshot() const override { return owner_.wifi_.Snapshot(); }
        void SetStateChangeSink(device::WifiStateChangeSink, void*) override;
        std::expected<void, device::WifiError> SetEnabled(bool enabled) override;
        std::expected<void, device::WifiError> RequestScan() override;
        std::expected<void, device::WifiError> ConnectSaved(std::string_view ssid) override;
        std::expected<void, device::WifiError> Connect(std::string_view ssid, std::string_view password) override;
        std::expected<void, device::WifiError> Disconnect() override;
        std::expected<void, device::WifiError> Forget(std::string_view ssid) override;

       private:
        NetworkController& owner_;
    };
    class CellularControlView final : public device::Cellular {
       public:
        explicit CellularControlView(NetworkController& owner) : owner_(owner) {}
        std::expected<void, device::CellularError> Initialize() override;
        device::CellularSnapshot Snapshot() const override { return owner_.cellular_.Snapshot(); }
        bool TryHoldConfiguration() override;
        void ReleaseConfiguration() override;
        void Poll() override {}
        void RequestSimRefresh() override { owner_.cellular_.RequestSimRefresh(); }
        void SetStateChangeSink(device::CellularStateChangeSink, void*) override;
        std::expected<void, device::CellularError> SetEnabled(bool enabled) override;
        std::expected<void, device::CellularError> SetSimSlot(device::CellularSimSlot slot) override;

       private:
        NetworkController& owner_;
    };
    static void Changed(void* context);
    void Notify();
    device::Wifi& wifi_;
    device::Cellular& cellular_;
    device::NetworkRouteSource& routes_;
    WifiControlView wifi_control_{*this};
    CellularControlView cellular_control_{*this};
    mutable std::mutex snapshot_mutex_;
    mutable device::WifiSnapshot wifi_workspace_{};
    mutable device::NetworkRoute previous_route_{};
    mutable uint64_t route_generation_{};
    std::mutex configuration_mutex_;
    bool update_active_{};
    std::mutex sink_mutex_;
    NetworkStateChangeSink sink_{};
    void* sink_context_{};
};

}  // namespace micropixel::host::network
