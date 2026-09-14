// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace micropixel::host {

// The install request has its own lifetime, independent of Store checks.
enum class StoreUpdateRequestState : uint8_t { kIdle, kRequesting, kQueued, kFailed };

constexpr bool StoreUpdateRequestBusy(StoreUpdateRequestState state) {
    return state == StoreUpdateRequestState::kRequesting || state == StoreUpdateRequestState::kQueued;
}

}  // namespace micropixel::host
