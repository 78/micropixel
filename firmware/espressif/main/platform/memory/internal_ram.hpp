// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "esp_memory_utils.h"

namespace micropixel::platform::memory {

// Check the entire object before registering an IRAM interrupt that accesses
// it. Embedded queues and records belong to the same internal allocation;
// pointers followed by the ISR still require their own ownership audit.
template <typename T>
[[nodiscard]] bool IsInternalObject(const T& object) {
    const auto* begin = reinterpret_cast<const uint8_t*>(&object);
    return esp_ptr_internal(begin) && esp_ptr_internal(begin + sizeof(T) - 1U);
}

}  // namespace micropixel::platform::memory
