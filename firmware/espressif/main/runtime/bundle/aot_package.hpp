#ifndef MICROPIXEL_RUNTIME_BUNDLE_AOT_PACKAGE_HPP
#define MICROPIXEL_RUNTIME_BUNDLE_AOT_PACKAGE_HPP

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

#include "runtime/bundle/bundle_reader.h"
#include "runtime/bundle/bundle_source.h"
#include "runtime/bundlefs/bundlefs.h"

namespace micropixel::runtime {

enum class AotPackageError {
    kOpenFailed,
};

constexpr uint32_t kMaxInstalledApps = BUNDLEFS_MAX_FILES;

// Which Bundle store holds an installed App: the system store (NOR app_store,
// Components and factory Apps) or the board's external App storage medium.
enum class AppStorage : uint8_t {
    kSystem,
    kExternal,
};

// Mount state of the optional external App storage as seen by the last
// catalog load or format. Only kReady stores receive downloaded Apps.
enum class ExternalStorageState : uint8_t {
    kAbsent,
    kReady,
    kNotFormatted,
    kUnsupportedFormat,
    kCorrupt,
    kUnavailable,
};

struct InstalledApp final {
    std::array<char, MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U> app_id{};
    std::array<char, MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH + 1U> display_name{};
    std::array<char, 32U> version{};
    uint32_t display_profile{MICROPIXEL_BUNDLE_DISPLAY_SQUARE};
    uint32_t bundle_size{};
    uint32_t content_id{};
    std::array<uint8_t, BUNDLEFS_SHA256_SIZE> sha256{};
    AppStorage storage{AppStorage::kSystem};
    // Where the Bundle bytes come from; readers do not care which storage.
    micropixel_bundle_source_t source{};
};

struct StorageUsage final {
    uint64_t total_bytes{};
    uint64_t used_bytes{};
};

struct InstalledAppCatalog final {
    std::array<InstalledApp, kMaxInstalledApps> apps{};
    uint32_t count{};
    uint32_t component_count{};
    // Sum over every mounted store; per-store figures follow.
    uint64_t store_total_bytes{};
    uint64_t store_used_bytes{};
    StorageUsage system_storage{};
    StorageUsage external_storage{};
    ExternalStorageState external_state{ExternalStorageState::kAbsent};

    // Re-initializes in place. The catalog is tens of KiB, so `catalog = {}`
    // may first build a second catalog on the caller's task stack.
    void Reset() { std::construct_at(this); }
};

class AppStore;

[[nodiscard]] std::expected<void, AotPackageError> ScanInstalledApps(AppStore& store, InstalledAppCatalog& catalog_out,
                                                                     std::string_view effective_locale = "en");

// Owns one addressable launch asset used as the first App Hall cover.
// Unlike AotPackage, it never makes the complete Bundle addressable.
class LaunchAssetMapping final {
   public:
    LaunchAssetMapping() = default;
    LaunchAssetMapping(const LaunchAssetMapping&) = delete;
    LaunchAssetMapping& operator=(const LaunchAssetMapping&) = delete;
    LaunchAssetMapping(LaunchAssetMapping&& other) noexcept;
    LaunchAssetMapping& operator=(LaunchAssetMapping&& other) noexcept;
    ~LaunchAssetMapping();

    [[nodiscard]] static std::expected<LaunchAssetMapping, AotPackageError> Open(
        const micropixel_bundle_source_t& source);
    [[nodiscard]] bool valid() const {  // NOLINT(readability-identifier-naming)
        return mapping_.mapping.data != nullptr;
    }
    [[nodiscard]] const micropixel_bundle_asset_view_t& asset() const {  // NOLINT(readability-identifier-naming)
        return mapping_.asset;
    }

   private:
    void Reset();

    micropixel_bundle_asset_mapping_t mapping_{};
};

// Owns both the copied AOT payload and the read-only Bundle view.
class AotPackage final {
   public:
    AotPackage(const AotPackage&) = delete;
    AotPackage& operator=(const AotPackage&) = delete;
    AotPackage(AotPackage&& other) noexcept;
    AotPackage& operator=(AotPackage&& other) noexcept;
    ~AotPackage();

    [[nodiscard]] static std::expected<AotPackage, AotPackageError> Load(const micropixel_bundle_source_t& source);

    [[nodiscard]] uint8_t* data() const;                        // NOLINT(readability-identifier-naming)
    [[nodiscard]] uint32_t size() const;                        // NOLINT(readability-identifier-naming)
    [[nodiscard]] const micropixel_aot_package_t& raw() const;  // NOLINT(readability-identifier-naming)

   private:
    AotPackage() = default;
    void Reset();

    micropixel_aot_package_t package_{};
};

}  // namespace micropixel::runtime

#endif
