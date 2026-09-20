// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "esp_err.h"

namespace micropixel::runtime {

struct AppStorageUsage final {
    uint32_t partition_bytes{};
    // Allocated NVS entries include record/namespace overhead, not just values.
    uint32_t used_bytes{};
    // Entry space after NVS reserves its GC page; not a guaranteed blob size.
    uint32_t available_bytes{};
};

[[nodiscard]] std::expected<AppStorageUsage, esp_err_t> ReadAppStorageUsage();

[[nodiscard]] bool AppStorageNamespace(std::string_view app_id, std::span<char> output);
// Caller must ensure no AppSession exists. A missing namespace is already clean.
[[nodiscard]] esp_err_t EraseAppStorage(std::string_view app_id);

}  // namespace micropixel::runtime
