// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>

#include "runtime/bundle/bundle_format.h"
#include "runtime/bundlefs/bundlefs.h"

namespace micropixel::runtime {
// Covers both mounted stores, independently of the launchable App limit.
inline constexpr uint32_t kMaxInstalledPackages = 2U * BUNDLEFS_MAX_FILES;
struct InstalledPackage final {
    std::array<char, 65U> app_id{};
    std::array<char, 32U> version{};
    std::array<char, MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH + 1U> display_name{};
    uint32_t bundle_size{};
    bool component{};
    bool external_storage{};
    std::array<uint8_t, BUNDLEFS_SHA256_SIZE> sha256{};
};
// Caller-owned PSRAM workspace; never return this inventory by value.
struct PackageInventory final {
    std::array<InstalledPackage, kMaxInstalledPackages> packages{};
    uint32_t count{};
};
}  // namespace micropixel::runtime
