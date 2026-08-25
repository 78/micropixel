#include "runtime/bundle/aot_package.hpp"

#include <cstdio>
#include <utility>

#include "runtime/bundle/app_store.hpp"

namespace micropixel::runtime {

std::expected<void, AotPackageError> ScanInstalledApps(InstalledAppCatalog& catalog_out) {
    if (!LoadAppStoreCatalog(catalog_out)) {
        return std::unexpected(AotPackageError::kOpenFailed);
    }
    return {};
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

std::expected<LaunchAssetMapping, AotPackageError> LaunchAssetMapping::Open(const bundlefs_file_t& file) {
    LaunchAssetMapping mapping;
    if (!micropixel_open_launch_asset(&file, &mapping.mapping_)) {
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

std::expected<AotPackage, AotPackageError> AotPackage::Load(const bundlefs_file_t& file) {
    AotPackage package;
    if (!micropixel_open_aot_package(&file, &package.package_)) {
        return std::unexpected(AotPackageError::kOpenFailed);
    }
    return package;
}

uint8_t* AotPackage::data() const { return const_cast<uint8_t*>(package_.payload); }

uint32_t AotPackage::size() const { return package_.payload_size; }

const micropixel_aot_package_t& AotPackage::raw() const { return package_; }

void AotPackage::Reset() { micropixel_close_aot_package(&package_); }

}  // namespace micropixel::runtime
