#pragma once
#include "device/contracts/network_route.hpp"

namespace micropixel::platform::network {
class NetworkRouteSource final : public device::NetworkRouteSource {
   public:
    void Read(device::NetworkRoute& destination) const override;
};
}  // namespace micropixel::platform::network
