#ifndef MICROPIXEL_RUNTIME_BUNDLE_APP_STORE_HPP
#define MICROPIXEL_RUNTIME_BUNDLE_APP_STORE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

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
    kUntrustedComponent,
    kComponentActive,
};

struct AppInstallRequest final {
    const uint8_t* data{};
    size_t size{};
    const char* expected_app_id{};
    std::array<uint8_t, 32U> expected_sha256{};
    // Set only after a future trusted publisher signature verifier succeeds.
    // Current remote/local App install paths leave this false.
    bool trusted_component_signature{};
};

struct AppInstallResult final {
    InstalledApp app{};
    uint32_t package_type{MICROPIXEL_BUNDLE_PACKAGE_APP};
    bool changed{};
};

// InstalledAppCatalog contains up to fifty opaque BundleFS handles and is too large
// to return by value on the bounded Host supervisor stack. The caller owns its
// storage so production paths can keep it in PSRAM.
[[nodiscard]] std::expected<void, AppStoreError> LoadAppStoreCatalog(InstalledAppCatalog& catalog_out,
                                                                     std::string_view effective_locale = "en");
[[nodiscard]] std::expected<AppInstallResult, AppStoreError> InstallApp(const AppInstallRequest& request,
                                                                        std::string_view effective_locale = "en");
[[nodiscard]] std::expected<void, AppStoreError> UninstallApp(const char* app_id);
[[nodiscard]] std::expected<void, AppStoreError> UninstallComponent(const char* component_id,
                                                                    std::string_view active_component_id = {});

}  // namespace micropixel::runtime

#endif
