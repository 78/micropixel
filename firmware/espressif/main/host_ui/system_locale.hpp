#ifndef MICROPIXEL_HOST_UI_SYSTEM_LOCALE_HPP
#define MICROPIXEL_HOST_UI_SYSTEM_LOCALE_HPP

#include <array>
#include <span>
#include <string_view>

#include "abi/micropixel_abi.h"

namespace micropixel::host_ui {

inline constexpr std::string_view kDefaultLocale = "en";
using LocaleTagBuffer = std::array<char, MICROPIXEL_LOCALE_TAG_MAX_BYTES + 1U>;

// Owns the user's persisted choice separately from the Locale that the current
// Host catalog and font set can actually render. Both values are normalized
// BCP 47 tags from the deliberately small v1 subset.
class SystemLocaleState final {
   public:
    SystemLocaleState();

    [[nodiscard]] bool SetRequested(std::string_view tag);
    void ResolveEffective(std::span<const std::string_view> available_locales);

    [[nodiscard]] std::string_view requested() const;  // NOLINT(readability-identifier-naming)
    [[nodiscard]] std::string_view effective() const;  // NOLINT(readability-identifier-naming)
    [[nodiscard]] const char* requested_c_str() const {
        return requested_.data();
    }  // NOLINT(readability-identifier-naming)
    [[nodiscard]] const char* effective_c_str() const {
        return effective_.data();
    }  // NOLINT(readability-identifier-naming)

   private:
    LocaleTagBuffer requested_{};
    LocaleTagBuffer effective_{};
};

// Normalizes language[-Script][-REGION]. v1 intentionally rejects variants,
// extensions and private-use subtags until matching semantics are specified.
[[nodiscard]] bool NormalizeLocaleTag(std::string_view input, LocaleTagBuffer& output);

[[nodiscard]] const char* LocaleDisplayName(std::string_view tag);

}  // namespace micropixel::host_ui

#endif
