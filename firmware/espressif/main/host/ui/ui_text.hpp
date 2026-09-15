// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "host/ui/system_locale.hpp"
#include "host_strings.hpp"

namespace micropixel::host_ui {
[[nodiscard]] inline const char* UiText(host_strings::Id id) { return host_strings::ForTag(DisplayLocale()).Get(id); }
[[nodiscard]] inline const char* UiLocaleName(std::string_view locale) {
    using Id = host_strings::Id;
    const auto id = locale == "zh-CN"   ? Id::kLanguageSimplifiedChinese
                    : locale == "zh-TW" ? Id::kLanguageTraditionalChinese
                    : locale == "ja-JP" ? Id::kLanguageJapanese
                    : locale == "ko-KR" ? Id::kLanguageKorean
                                        : Id::kLanguageEnglish;
    return UiText(id);
}
}  // namespace micropixel::host_ui
