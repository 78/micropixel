#ifndef MICROPIXEL_RUNTIME_BUNDLE_APP_STORE_HPP
#define MICROPIXEL_RUNTIME_BUNDLE_APP_STORE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>

#include "runtime/bundle/aot_package.hpp"

namespace micropixel::runtime {

enum class AppStoreError : uint8_t {
    kUnavailable,
    kCatalogCorrupt,
    kInvalidPackage,
    kHashMismatch,
    kAppIdMismatch,
    kCatalogFull,
    kNoSpace,
    kFlashWrite,
    kCommitFailed,
    kNotFound,
};

struct AppInstallRequest final {
    const uint8_t* data{};
    size_t size{};
    const char* expected_app_id{};
    std::array<uint8_t, 32U> expected_sha256{};
};

[[nodiscard]] std::expected<InstalledAppCatalog, AppStoreError> LoadAppStoreCatalog();
[[nodiscard]] std::expected<InstalledApp, AppStoreError> InstallApp(const AppInstallRequest& request);
[[nodiscard]] std::expected<void, AppStoreError> UninstallApp(const char* app_id);

}  // namespace micropixel::runtime

#endif
