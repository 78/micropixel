#include "host/network/network_controller.hpp"

#include <new>

namespace micropixel::host::network {

NetworkController::NetworkController(device::Wifi& wifi, device::Cellular& cellular, device::NetworkRouteSource& routes)
    : wifi_(wifi), cellular_(cellular), routes_(routes) {
    wifi_.SetStateChangeSink(Changed, this);
    cellular_.SetStateChangeSink(Changed, this);
}
NetworkController::~NetworkController() {
    wifi_.SetStateChangeSink(nullptr, nullptr);
    cellular_.SetStateChangeSink(nullptr, nullptr);
    EndFirmwareUpdate();
}
void NetworkController::CopySnapshot(NetworkSnapshot& out) const {
    std::lock_guard lock(snapshot_mutex_);
    // Construct directly in the owned workspace; WifiSnapshot contains scan lists.
    wifi_workspace_.~WifiSnapshot();
    new (&wifi_workspace_) device::WifiSnapshot(wifi_.Snapshot());
    out.wifi = {.available = wifi_workspace_.available,
                .enabled = wifi_workspace_.enabled,
                .connected = wifi_workspace_.connected};
    for (uint32_t i = 0; i < wifi_workspace_.saved_network_count; ++i) {
        const auto& entry = wifi_workspace_.saved_networks[i];
        if (entry.connected) {
            out.wifi.ssid = entry.ssid;
            out.wifi.rssi = entry.rssi;
            break;
        }
    }
    out.cellular.~CellularSnapshot();
    new (&out.cellular) device::CellularSnapshot(cellular_.Snapshot());
    routes_.Read(out.route);
    if (out.route != previous_route_) {
        previous_route_ = out.route;
        ++route_generation_;
    }
    out.route_generation = route_generation_;
    out.available = out.wifi.available || out.cellular.available;
    out.enabled = out.wifi.enabled || out.cellular.enabled;
    out.connected = out.route.transport != device::NetworkTransport::kNone;
}
void NetworkController::SetStateChangeSink(NetworkStateChangeSink sink, void* context) {
    std::lock_guard lock(sink_mutex_);
    sink_ = sink;
    sink_context_ = context;
}
void NetworkController::Changed(void* context) { static_cast<NetworkController*>(context)->Notify(); }
void NetworkController::Notify() {
    std::lock_guard lock(sink_mutex_);
    if (sink_) sink_(sink_context_);
}
void NetworkController::Poll() {
    wifi_.Poll();
    cellular_.Poll();
    Notify();
}
bool NetworkController::TryBeginFirmwareUpdate() {
    std::lock_guard lock(configuration_mutex_);
    if (update_active_) return false;
    {
        std::lock_guard snapshot_lock(snapshot_mutex_);
        wifi_workspace_.~WifiSnapshot();
        new (&wifi_workspace_) device::WifiSnapshot(wifi_.Snapshot());
        if (wifi_workspace_.control_pending ||
            wifi_workspace_.connection_state == device::WifiConnectionState::kConnecting)
            return false;
    }
    if (!cellular_.TryHoldConfiguration()) return false;
    update_active_ = true;
    return true;
}
void NetworkController::EndFirmwareUpdate() {
    std::lock_guard lock(configuration_mutex_);
    if (!update_active_) return;
    cellular_.ReleaseConfiguration();
    update_active_ = false;
}
std::expected<void, device::WifiError> NetworkController::WifiControlView::Initialize() {
    std::lock_guard lock(owner_.configuration_mutex_);
    if (owner_.update_active_) return std::unexpected(device::WifiError::kBusy);
    return owner_.wifi_.Initialize();
}
void NetworkController::WifiControlView::SetStateChangeSink(device::WifiStateChangeSink sink, void* context) {
    owner_.SetStateChangeSink(sink, context);
}
std::expected<void, device::WifiError> NetworkController::WifiControlView::SetEnabled(bool enabled) {
    std::lock_guard lock(owner_.configuration_mutex_);
    if (owner_.update_active_) return std::unexpected(device::WifiError::kBusy);
    return owner_.wifi_.SetEnabled(enabled);
}
std::expected<void, device::WifiError> NetworkController::WifiControlView::RequestScan() {
    return owner_.wifi_.RequestScan();
}
std::expected<void, device::WifiError> NetworkController::WifiControlView::ConnectSaved(std::string_view ssid) {
    std::lock_guard lock(owner_.configuration_mutex_);
    if (owner_.update_active_) return std::unexpected(device::WifiError::kBusy);
    return owner_.wifi_.ConnectSaved(ssid);
}
std::expected<void, device::WifiError> NetworkController::WifiControlView::Connect(std::string_view ssid,
                                                                                   std::string_view password) {
    std::lock_guard lock(owner_.configuration_mutex_);
    if (owner_.update_active_) return std::unexpected(device::WifiError::kBusy);
    return owner_.wifi_.Connect(ssid, password);
}
std::expected<void, device::WifiError> NetworkController::WifiControlView::Disconnect() {
    std::lock_guard lock(owner_.configuration_mutex_);
    if (owner_.update_active_) return std::unexpected(device::WifiError::kBusy);
    return owner_.wifi_.Disconnect();
}
std::expected<void, device::WifiError> NetworkController::WifiControlView::Forget(std::string_view ssid) {
    std::lock_guard lock(owner_.configuration_mutex_);
    if (owner_.update_active_) return std::unexpected(device::WifiError::kBusy);
    return owner_.wifi_.Forget(ssid);
}
std::expected<void, device::CellularError> NetworkController::CellularControlView::Initialize() {
    std::lock_guard lock(owner_.configuration_mutex_);
    if (owner_.update_active_) return std::unexpected(device::CellularError::kBusy);
    return owner_.cellular_.Initialize();
}
bool NetworkController::CellularControlView::TryHoldConfiguration() { return owner_.TryBeginFirmwareUpdate(); }
void NetworkController::CellularControlView::ReleaseConfiguration() { owner_.EndFirmwareUpdate(); }
void NetworkController::CellularControlView::SetStateChangeSink(device::CellularStateChangeSink sink, void* context) {
    owner_.SetStateChangeSink(sink, context);
}
std::expected<void, device::CellularError> NetworkController::CellularControlView::SetEnabled(bool enabled) {
    std::lock_guard lock(owner_.configuration_mutex_);
    if (owner_.update_active_) return std::unexpected(device::CellularError::kBusy);
    return owner_.cellular_.SetEnabled(enabled);
}
std::expected<void, device::CellularError> NetworkController::CellularControlView::SetSimSlot(
    device::CellularSimSlot slot) {
    std::lock_guard lock(owner_.configuration_mutex_);
    if (owner_.update_active_) return std::unexpected(device::CellularError::kBusy);
    return owner_.cellular_.SetSimSlot(slot);
}
}  // namespace micropixel::host::network
