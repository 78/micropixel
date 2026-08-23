#ifndef MICROPIXEL_SDK_STORAGE_HPP
#define MICROPIXEL_SDK_STORAGE_HPP

#include <stdint.h>

#include "sdk/result.hpp"

namespace micropixel {

class Application;

// A lightweight view over the current Bundle AppId's private namespace.
// All mutations are synchronously committed by the Host before returning.
class KVStore final {
   public:
    [[nodiscard]] Result<uint32_t> GetU32(const char* key) const;
    [[nodiscard]] Result<void> SetU32(const char* key, uint32_t value) const;
    [[nodiscard]] Result<bool> GetBool(const char* key) const;
    [[nodiscard]] Result<void> SetBool(const char* key, bool value) const;
    [[nodiscard]] Result<uint32_t> GetBytes(const char* key, uint8_t* bytes, uint32_t capacity) const;
    [[nodiscard]] Result<void> SetBytes(const char* key, const uint8_t* bytes, uint32_t length) const;
    [[nodiscard]] Result<void> Remove(const char* key) const;

   private:
    struct CapabilityToken {
       private:
        friend class Application;
        constexpr CapabilityToken() noexcept = default;
    };

    friend class Application;
    explicit constexpr KVStore(CapabilityToken) noexcept {}
};

}  // namespace micropixel

#endif
