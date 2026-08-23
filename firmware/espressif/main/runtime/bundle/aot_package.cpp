#include "runtime/bundle/aot_package.hpp"

#include <cstdio>
#include <utility>

namespace micropixel::runtime {

std::expected<InstalledAppCatalog, AotPackageError> ScanInstalledApps() {
    micropixel_bundle_metadata_t metadata[kMaxInstalledApps]{};
    uint32_t count = 0U;
    if (!micropixel_scan_app_store(metadata, kMaxInstalledApps, &count)) {
        return std::unexpected(AotPackageError::kOpenFailed);
    }

    InstalledAppCatalog catalog{.count = count};
    for (uint32_t index = 0U; index < count; ++index) {
        const auto& source = metadata[index];
        auto& destination = catalog.apps[index];
        (void)std::snprintf(destination.app_id.data(), destination.app_id.size(), "%s",
                            reinterpret_cast<const char*>(source.app_id));
        (void)std::snprintf(destination.display_name.data(), destination.display_name.size(), "%s",
                            reinterpret_cast<const char*>(source.display_name));
        destination.store_offset = source.store_offset;
        destination.bundle_size = source.bundle_size;
    }
    return catalog;
}

LaunchAssetMapping::LaunchAssetMapping(LaunchAssetMapping&& other) noexcept
    : mapping_(std::exchange(other.mapping_, {})) {}

LaunchAssetMapping& LaunchAssetMapping::operator=(LaunchAssetMapping&& other) noexcept {
    if (this != &other) {
        Reset();
        mapping_ = std::exchange(other.mapping_, {});
    }
    return *this;
}

LaunchAssetMapping::~LaunchAssetMapping() { Reset(); }

std::expected<LaunchAssetMapping, AotPackageError> LaunchAssetMapping::Open(uint32_t store_offset) {
    LaunchAssetMapping mapping;
    if (!micropixel_open_launch_asset(store_offset, &mapping.mapping_)) {
        return std::unexpected(AotPackageError::kOpenFailed);
    }
    return mapping;
}

void LaunchAssetMapping::Reset() { micropixel_close_asset_mapping(&mapping_); }

AotPackage::AotPackage(AotPackage&& other) noexcept : package_(std::exchange(other.package_, {})) {}

AotPackage& AotPackage::operator=(AotPackage&& other) noexcept {
    if (this != &other) {
        Reset();
        package_ = std::exchange(other.package_, {});
    }
    return *this;
}

AotPackage::~AotPackage() { Reset(); }

std::expected<AotPackage, AotPackageError> AotPackage::Load(uint32_t store_offset) {
    AotPackage package;
    if (!micropixel_open_aot_package(store_offset, &package.package_)) {
        return std::unexpected(AotPackageError::kOpenFailed);
    }
    return package;
}

uint8_t* AotPackage::data() const { return const_cast<uint8_t*>(package_.payload); }

uint32_t AotPackage::size() const { return package_.payload_size; }

const micropixel_aot_package_t& AotPackage::raw() const { return package_; }

void AotPackage::Reset() { micropixel_close_aot_package(&package_); }

}  // namespace micropixel::runtime
