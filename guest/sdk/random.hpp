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
    // Returns a uniform value in [0, upper_bound) without modulo bias.
    // upper_bound must be greater than zero.
    [[nodiscard]] uint32_t Below(uint32_t upper_bound) const;

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
