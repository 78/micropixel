#include "host/network/async_wifi.hpp"

#include <algorithm>
#include <new>

namespace micropixel::host::network {
AsyncWifi::AsyncWifi(device::Wifi& driver, work::BackgroundExecutor& worker) : driver_(driver), worker_(worker) {
    driver_.SetStateChangeSink(Changed, this);
    Poll();
}
AsyncWifi::~AsyncWifi() {
    driver_.SetStateChangeSink(nullptr, nullptr);
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        sink_ = nullptr;
    }
    worker_.Shutdown();
}
device::WifiSnapshot AsyncWifi::Snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}
void AsyncWifi::SetStateChangeSink(device::WifiStateChangeSink sink, void* context) {
    std::lock_guard lock(mutex_);
    sink_ = sink;
    sink_context_ = context;
}
void AsyncWifi::NotifyLocked() {
    if (sink_) sink_(sink_context_);
}
void AsyncWifi::Changed(void* context) { static_cast<AsyncWifi*>(context)->Poll(); }
bool AsyncWifi::ScheduleLocked() {
    if (scheduled_) return true;
    scheduled_ = worker_.Submit(Run, this);
    return scheduled_;
}
void AsyncWifi::Poll() {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    refresh_needed_ = true;
    (void)ScheduleLocked();
}
std::expected<void, device::WifiError> AsyncWifi::Submit(Operation operation, bool enabled, std::string_view ssid,
                                                         std::string_view password) {
    if (ssid.size() > device::kWifiSsidCapacity || password.size() > device::kWifiPasswordCapacity ||
        ((operation == Operation::kConnect || operation == Operation::kConnectSaved ||
          operation == Operation::kForget) &&
         ssid.empty()))
        return std::unexpected(device::WifiError::kInvalidArgument);
    std::lock_guard lock(mutex_);
    if (stopping_ || (!snapshot_.available && operation != Operation::kInitialize))
        return std::unexpected(device::WifiError::kUnavailable);
    if (snapshot_.control_pending) return std::unexpected(device::WifiError::kBusy);
    if (operation == Operation::kEnable && enabled == snapshot_.enabled) return {};
    command_ = {};
    command_.operation = operation;
    command_.enabled = enabled;
    std::copy(ssid.begin(), ssid.end(), command_.ssid.begin());
    std::copy(password.begin(), password.end(), command_.password.begin());
    if (!ScheduleLocked()) {
        command_ = {};
        return std::unexpected(device::WifiError::kBusy);
    }
    snapshot_.control_pending = true;
    snapshot_.control_failed = false;
    if (operation == Operation::kEnable) snapshot_.enabled = enabled;
    NotifyLocked();
    return {};
}
std::expected<void, device::WifiError> AsyncWifi::Initialize() { return Submit(Operation::kInitialize); }
std::expected<void, device::WifiError> AsyncWifi::SetEnabled(bool enabled) {
    return Submit(Operation::kEnable, enabled);
}
std::expected<void, device::WifiError> AsyncWifi::RequestScan() { return Submit(Operation::kScan); }
std::expected<void, device::WifiError> AsyncWifi::ConnectSaved(std::string_view ssid) {
    return Submit(Operation::kConnectSaved, false, ssid);
}
std::expected<void, device::WifiError> AsyncWifi::Connect(std::string_view ssid, std::string_view password) {
    return Submit(Operation::kConnect, false, ssid, password);
}
std::expected<void, device::WifiError> AsyncWifi::Disconnect() { return Submit(Operation::kDisconnect); }
std::expected<void, device::WifiError> AsyncWifi::Forget(std::string_view ssid) {
    return Submit(Operation::kForget, false, ssid);
}
void AsyncWifi::Run(void* context) { static_cast<AsyncWifi*>(context)->Drain(); }
void AsyncWifi::Drain() {
    for (;;) {
        Command command;
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || (command_.operation == Operation::kNone && !refresh_needed_)) {
                scheduled_ = false;
                return;
            }
            command = command_;
            command_ = {};
            refresh_needed_ = false;
        }
        std::expected<void, device::WifiError> result{};
        switch (command.operation) {
            case Operation::kNone:
                break;
            case Operation::kInitialize:
                result = driver_.Initialize();
                break;
            case Operation::kEnable:
                result = driver_.SetEnabled(command.enabled);
                break;
            case Operation::kScan:
                result = driver_.RequestScan();
                break;
            case Operation::kConnectSaved:
                result = driver_.ConnectSaved(command.ssid.data());
                break;
            case Operation::kConnect:
                result = driver_.Connect(command.ssid.data(), command.password.data());
                break;
            case Operation::kDisconnect:
                result = driver_.Disconnect();
                break;
            case Operation::kForget:
                result = driver_.Forget(command.ssid.data());
                break;
        }
        workspace_.~WifiSnapshot();
        new (&workspace_) device::WifiSnapshot(driver_.Snapshot());
        {
            std::lock_guard lock(mutex_);
            // A command may have arrived while a read-only RPC was running.
            // Keep its busy/desired state until that command actually completes.
            const bool pending = snapshot_.control_pending && command.operation == Operation::kNone;
            const bool desired_enabled = snapshot_.enabled;
            const bool failed = command.operation == Operation::kNone ? snapshot_.control_failed : !result;
            snapshot_ = workspace_;
            snapshot_.control_pending = pending;
            snapshot_.control_failed = failed;
            if (pending && command_.operation == Operation::kEnable) snapshot_.enabled = desired_enabled;
            NotifyLocked();
        }
    }
}
}  // namespace micropixel::host::network
