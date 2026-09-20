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

    // True when an argument is exactly `name` or starts with "name=". Use it
    // for boolean switches such as "--benchmark" or "--no-bgm".
    [[nodiscard]] bool HasFlag(const char* name) const {
        for (uint32_t index = 0U; index < count(); ++index) {
            const char* argument = Get(index);
            if (argument == nullptr) {
                continue;
            }
            uint32_t offset = 0U;
            while (name[offset] != '\0' && argument[offset] == name[offset]) {
                ++offset;
            }
            if (name[offset] == '\0' && (argument[offset] == '\0' || argument[offset] == '=')) {
                return true;
            }
        }
        return false;
    }

    // Decimal value of "--name N" / "--name=N"; `fallback` when the option is
    // absent, empty, not all digits or does not fit in 32 bits.
    [[nodiscard]] uint32_t GetUnsigned(const char* name, uint32_t fallback) const {
        const char* text = FindValue(name);
        if (text == nullptr || *text == '\0') {
            return fallback;
        }
        uint32_t value = 0U;
        for (; *text != '\0'; ++text) {
            if (*text < '0' || *text > '9') {
                return fallback;
            }
            const auto digit = static_cast<uint32_t>(*text - '0');
            if (value > (0xFFFFFFFFU - digit) / 10U) {
                return fallback;
            }
            value = value * 10U + digit;
        }
        return value;
    }

   private:
    struct CapabilityToken {};
    explicit constexpr LaunchArguments(CapabilityToken) {}
    friend class Application;
};

}  // namespace micropixel

#endif
