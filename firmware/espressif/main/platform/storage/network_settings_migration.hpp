// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"

namespace micropixel::platform::storage {

// Called once at startup with both NVS partitions initialized, before any
// network service or Guest opens settings. Never erases a whole namespace.
[[nodiscard]] esp_err_t MigrateNetworkSettings();

}  // namespace micropixel::platform::storage
