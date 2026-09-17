#include <atomic>
#include <cassert>
#include <cstring>
#include <thread>

#include "host/network/network_controller.hpp"

using namespace micropixel;
namespace {
struct Wifi final : device::Wifi {
    device::WifiSnapshot snapshot{};
    device::WifiStateChangeSink sink{};
    void* context{};
    int mutations{};
    std::atomic<bool> block{}, entered{}, release{};
    std::expected<void, device::WifiError> Initialize() override { return {}; }
    device::WifiSnapshot Snapshot() const override { return snapshot; }
    void SetStateChangeSink(device::WifiStateChangeSink s, void* c) override {
        sink = s;
        context = c;
    }
    std::expected<void, device::WifiError> SetEnabled(bool enabled) override {
        if (block) {
            entered = true;
            while (!release) std::this_thread::yield();
        }
        ++mutations;
        snapshot.enabled = enabled;
        if (sink) sink(context);
        return {};
    }
    std::expected<void, device::WifiError> RequestScan() override { return {}; }
    std::expected<void, device::WifiError> ConnectSaved(std::string_view) override {
        ++mutations;
        return {};
    }
    std::expected<void, device::WifiError> Connect(std::string_view, std::string_view) override {
        ++mutations;
        return {};
    }
    std::expected<void, device::WifiError> Disconnect() override {
        ++mutations;
        return {};
    }
    std::expected<void, device::WifiError> Forget(std::string_view) override {
        ++mutations;
        return {};
    }
};
struct Cellular final : device::Cellular {
    device::CellularSnapshot snapshot{};
    device::CellularStateChangeSink sink{};
    void* context{};
    bool held{}, busy{};
    int polls{}, mutations{}, reads{};
    std::expected<void, device::CellularError> Initialize() override { return {}; }
    device::CellularSnapshot Snapshot() const override { return snapshot; }
    bool TryHoldConfiguration() override {
        if (busy || held) return false;
        held = true;
        return true;
    }
    void ReleaseConfiguration() override { held = false; }
    void Poll() override { ++polls; }
    void RequestSimRefresh() override { ++reads; }
    void SetStateChangeSink(device::CellularStateChangeSink s, void* c) override {
        sink = s;
        context = c;
    }
    std::expected<void, device::CellularError> SetEnabled(bool) override {
        ++mutations;
        return {};
    }
    std::expected<void, device::CellularError> SetSimSlot(device::CellularSimSlot) override {
        ++mutations;
        return {};
    }
};
struct Routes final : device::NetworkRouteSource {
    device::NetworkRoute route{};
    void Read(device::NetworkRoute& out) const override { out = route; }
};
}  // namespace
int main() {
    Wifi wifi;
    Cellular cellular;
    Routes routes;
    {
        host::network::NetworkController network(wifi, cellular, routes);
        unsigned notifications{};
        network.SetStateChangeSink([](void* p) { ++*static_cast<unsigned*>(p); }, &notifications);
        host::network::NetworkSnapshot state;
        network.CopySnapshot(state);
        assert(!state.available && !state.enabled && !state.connected);
        wifi.snapshot.available = true;
        wifi.snapshot.enabled = true;
        wifi.snapshot.connected = true;
        wifi.snapshot.saved_network_count = 1;
        wifi.snapshot.saved_networks[0].connected = true;
        wifi.snapshot.saved_networks[0].rssi = -50;
        std::strcpy(wifi.snapshot.saved_networks[0].ssid.data(), "test");
        routes.route.transport = device::NetworkTransport::kWifi;
        std::strcpy(routes.route.address.data(), "192.0.2.1");
        network.CopySnapshot(state);
        assert(state.connected && state.route.transport == device::NetworkTransport::kWifi);
        const auto first = state.route_generation;
        cellular.snapshot = {.available = true, .enabled = true, .connected = true};
        network.CopySnapshot(state);
        assert(state.route_generation == first);  // A standby link and RSSI do not invalidate the active route.
        wifi.snapshot.saved_networks[0].rssi = -65;
        network.CopySnapshot(state);
        assert(state.wifi.rssi == -65 && state.route_generation == first);
        routes.route.transport = device::NetworkTransport::kCellular;
        network.CopySnapshot(state);
        assert(state.connected && state.route.transport == device::NetworkTransport::kCellular &&
               state.route_generation > first);
        // Use actual default route even while both device snapshots report connected.
        auto generation = state.route_generation;
        std::strcpy(routes.route.address.data(), "192.0.2.2");
        network.CopySnapshot(state);
        assert(state.route_generation > generation);
        wifi.snapshot.connected = false;
        wifi.snapshot.saved_network_count = 0;
        network.CopySnapshot(state);
        assert(state.connected && state.wifi.ssid[0] == 0);
        routes.route = {};
        network.CopySnapshot(state);
        assert(!state.connected);  // A stale link flag is not a usable route.
        wifi.sink(wifi.context);
        cellular.sink(cellular.context);
        network.Poll();
        assert(notifications == 3 && cellular.polls == 1);  // Maintenance does not need an open page.
        assert(network.TryBeginFirmwareUpdate());
        assert(!network.TryBeginFirmwareUpdate());
        assert(!network.WifiControl().SetEnabled(false));
        assert(!network.WifiControl().ConnectSaved("test"));
        assert(!network.WifiControl().Connect("test", "password"));
        assert(!network.WifiControl().Disconnect());
        assert(!network.WifiControl().Forget("test"));
        assert(!network.CellularControl().SetEnabled(false));
        assert(!network.CellularControl().SetSimSlot(device::CellularSimSlot::kInternal));
        network.CellularControl().RequestSimRefresh();
        assert(cellular.reads == 1 && wifi.mutations == 0 && cellular.mutations == 0);
        network.EndFirmwareUpdate();
        assert(!cellular.held && network.WifiControl().SetEnabled(false));
        cellular.busy = true;
        assert(!network.TryBeginFirmwareUpdate());
        assert(network.WifiControl().SetEnabled(true));  // Failed acquisition does not leak the Host gate.
        cellular.busy = false;
        wifi.snapshot.connection_state = device::WifiConnectionState::kConnecting;
        assert(!network.TryBeginFirmwareUpdate() && !cellular.held);
        wifi.snapshot.connection_state = device::WifiConnectionState::kDisconnected;
        wifi.block = true;
        std::atomic<bool> reserved{};
        std::thread changing([&] { assert(network.WifiControl().SetEnabled(false)); });
        while (!wifi.entered) std::this_thread::yield();
        std::thread updating([&] { reserved = network.TryBeginFirmwareUpdate(); });
        assert(!reserved);
        wifi.release = true;
        changing.join();
        updating.join();
        assert(reserved && !network.WifiControl().SetEnabled(true));
        // Destructor releases a live reservation and detaches device callbacks.
    }
    assert(!cellular.held && !wifi.sink && !cellular.sink);
}
