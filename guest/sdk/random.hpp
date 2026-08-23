#ifndef MICROPIXEL_SDK_RANDOM_HPP
#define MICROPIXEL_SDK_RANDOM_HPP

#include <stdint.h>

namespace micropixel {

class Application;

// Lightweight view of the Host random service. Values come from the selected
// platform's hardware RNG rather than a Guest-local deterministic generator.
class Random final {
   public:
    [[nodiscard]] uint32_t U32() const;

   private:
    struct CapabilityToken final {
       private:
        constexpr CapabilityToken() = default;
        friend class Application;
    };

    explicit constexpr Random(CapabilityToken) noexcept {}
    friend class Application;
};

}  // namespace micropixel

#endif
