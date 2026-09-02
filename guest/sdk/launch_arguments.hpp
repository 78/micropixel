#ifndef MICROPIXEL_SDK_LAUNCH_ARGUMENTS_HPP
#define MICROPIXEL_SDK_LAUNCH_ARGUMENTS_HPP

#include <stdint.h>

namespace micropixel {

// Read-only arguments supplied when this AppSession was created. Returned
// pointers remain valid until main() returns.
class LaunchArguments final {
   public:
    [[nodiscard]] uint32_t count() const;
    [[nodiscard]] const char* Get(uint32_t index) const;

    // Supports both "--name value" and "--name=value". Returns nullptr when
    // the option is absent or the separate value is missing.
    [[nodiscard]] const char* FindValue(const char* name) const;

   private:
    struct CapabilityToken {};
    explicit constexpr LaunchArguments(CapabilityToken) {}
    friend class Application;
};

}  // namespace micropixel

#endif
