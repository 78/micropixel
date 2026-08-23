#include "runtime/bundle/aot_package.hpp"

#include <utility>

namespace micropixel::runtime {

AotPackage::AotPackage(AotPackage&& other) noexcept : package_(std::exchange(other.package_, {})) {}

AotPackage& AotPackage::operator=(AotPackage&& other) noexcept {
    if (this != &other) {
        Reset();
        package_ = std::exchange(other.package_, {});
    }
    return *this;
}

AotPackage::~AotPackage() { Reset(); }

std::expected<AotPackage, AotPackageError> AotPackage::Load() {
    AotPackage package;
    if (!micropixel_open_aot_package(&package.package_)) {
        return std::unexpected(AotPackageError::kOpenFailed);
    }
    return package;
}

uint8_t* AotPackage::data() const { return const_cast<uint8_t*>(package_.payload); }

uint32_t AotPackage::size() const { return package_.payload_size; }

const micropixel_aot_package_t& AotPackage::raw() const { return package_; }

void AotPackage::Reset() { micropixel_close_aot_package(&package_); }

}  // namespace micropixel::runtime
