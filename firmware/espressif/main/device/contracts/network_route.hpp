#pragma once

#include <array>
#include <cstdint>

namespace micropixel::device {

enum class NetworkTransport : uint8_t { kNone, kWifi, kCellular };

// A copied default IPv4 route, never a borrowed platform netif pointer.
// A usable IP route does not imply that the public Internet is reachable.
struct NetworkRoute final {
    NetworkTransport transport{NetworkTransport::kNone};
    std::array<char, 16> address{}, gateway{}, netmask{}, dns{};
    std::array<char, 18> mac{};
    std::array<char, 256> hostname{};
    bool operator==(const NetworkRoute&) const = default;
};

class NetworkRouteSource {
   public:
    virtual ~NetworkRouteSource() = default;
    virtual void Read(NetworkRoute& destination) const = 0;
};

}  // namespace micropixel::device
