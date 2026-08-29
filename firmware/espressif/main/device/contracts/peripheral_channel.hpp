#pragma once

#include <cstdint>

namespace micropixel::device {

// PeripheralType-local channel chosen by a board driver. It is never exposed to a
// Guest and has no relationship with the public DeviceId assigned by Host.
using PeripheralChannelId = uint16_t;

}  // namespace micropixel::device
