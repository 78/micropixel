#pragma once

#include "device/contracts/cellular.hpp"
#include "device/contracts/network_route.hpp"
#include "device/contracts/wifi.hpp"

namespace micropixel::host::network {

struct WifiLink final {
    bool available{}, enabled{}, connected{};
    int8_t rssi{};
    std::array<char, device::kWifiSsidCapacity + 1U> ssid{};
};

struct NetworkSnapshot final {
    bool available{}, enabled{}, connected{};
    WifiLink wifi{};
    device::CellularSnapshot cellular{};
    device::NetworkRoute route{};
    uint64_t route_generation{};
};

using NetworkStateChangeSink = void (*)(void* context);

// Host connectivity and configuration coordination. Detailed Wi-Fi/SIM
// controls stay separate; callers receive guarded views of those contracts.
class Network {
   public:
    virtual ~Network() = default;
    virtual void CopySnapshot(NetworkSnapshot& destination) const = 0;
    virtual device::Wifi& WifiControl() = 0;
    virtual device::Cellular& CellularControl() = 0;
    // Nonblocking wakeup only; the sink must not call back into this service.
    virtual void SetStateChangeSink(NetworkStateChangeSink sink, void* context) = 0;
    [[nodiscard]] virtual bool TryBeginFirmwareUpdate() = 0;
    virtual void EndFirmwareUpdate() = 0;
};

inline const char* TransportName(device::NetworkTransport transport) {
    switch (transport) {
        case device::NetworkTransport::kWifi:
            return "wifi";
        case device::NetworkTransport::kCellular:
            return "cellular";
        default:
            return "none";
    }
}

}  // namespace micropixel::host::network
