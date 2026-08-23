#ifndef MICROPIXEL_RUNTIME_BUNDLE_AOT_PACKAGE_HPP
#define MICROPIXEL_RUNTIME_BUNDLE_AOT_PACKAGE_HPP

#include <cstdint>
#include <expected>

#include "runtime/bundle/bundle_reader.h"

namespace micropixel::runtime {

enum class AotPackageError {
    kOpenFailed,
};

// Owns both the copied AOT payload and the optional read-only Bundle mapping.
class AotPackage final {
   public:
    AotPackage(const AotPackage&) = delete;
    AotPackage& operator=(const AotPackage&) = delete;
    AotPackage(AotPackage&& other) noexcept;
    AotPackage& operator=(AotPackage&& other) noexcept;
    ~AotPackage();

    [[nodiscard]] static std::expected<AotPackage, AotPackageError> Load();

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
