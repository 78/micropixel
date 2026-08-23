#ifndef MICROPIXEL_SDK_CLOCK_HPP
#define MICROPIXEL_SDK_CLOCK_HPP

#include "sdk/types.hpp"

namespace micropixel {

class Application;

// Lightweight view of the single application Clock service for this Guest.
// Copies are equivalent and do not create independent clocks.
class Clock final {
   public:
    constexpr Clock(const Clock&) noexcept = default;
    constexpr Clock& operator=(const Clock&) noexcept = default;

    [[nodiscard]] TimePoint Now() const;

   private:
    struct CapabilityToken {};
    explicit constexpr Clock(CapabilityToken) noexcept {}
    friend class Application;
};

}  // namespace micropixel

#endif
