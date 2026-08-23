#ifndef MICROPIXEL_RUNTIME_BUNDLE_AOT_PACKAGE_HPP
#define MICROPIXEL_RUNTIME_BUNDLE_AOT_PACKAGE_HPP

#include <array>
#include <cstdint>
#include <expected>

#include "runtime/bundle/bundle_reader.h"

namespace micropixel::runtime {

enum class AotPackageError {
    kOpenFailed,
};

constexpr uint32_t kMaxInstalledApps = 3U;

struct InstalledApp final {
    std::array<char, MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U> app_id{};
    std::array<char, MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH + 1U> display_name{};
    uint32_t store_offset{};
    uint32_t bundle_size{};
};

struct InstalledAppCatalog final {
    std::array<InstalledApp, kMaxInstalledApps> apps{};
    uint32_t count{};
};

[[nodiscard]] std::expected<InstalledAppCatalog, AotPackageError> ScanInstalledApps();

// Owns one Flash-mapped launch asset used as the first App Hall cover.
// Unlike AotPackage, it never maps the complete Bundle.
class LaunchAssetMapping final {
   public:
    LaunchAssetMapping() = default;
    LaunchAssetMapping(const LaunchAssetMapping&) = delete;
    LaunchAssetMapping& operator=(const LaunchAssetMapping&) = delete;
    LaunchAssetMapping(LaunchAssetMapping&& other) noexcept;
    LaunchAssetMapping& operator=(LaunchAssetMapping&& other) noexcept;
    ~LaunchAssetMapping();

    [[nodiscard]] static std::expected<LaunchAssetMapping, AotPackageError> Open(uint32_t store_offset);
    [[nodiscard]] bool valid() const { return mapping_.mapping != nullptr; }  // NOLINT(readability-identifier-naming)
    [[nodiscard]] const micropixel_bundle_asset_view_t& asset() const {       // NOLINT(readability-identifier-naming)
        return mapping_.asset;
    }

   private:
    void Reset();

    micropixel_bundle_asset_mapping_t mapping_{};
};

// Owns both the copied AOT payload and the optional read-only Bundle mapping.
class AotPackage final {
   public:
    AotPackage(const AotPackage&) = delete;
    AotPackage& operator=(const AotPackage&) = delete;
    AotPackage(AotPackage&& other) noexcept;
    AotPackage& operator=(AotPackage&& other) noexcept;
    ~AotPackage();

    [[nodiscard]] static std::expected<AotPackage, AotPackageError> Load(uint32_t store_offset);

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
