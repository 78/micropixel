// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <string_view>

namespace micropixel::runtime {
// Exact Host-owned names, unavailable to App/Component installation or removal.
inline constexpr std::array<std::string_view, 5> kSystemFontFiles{".font.en", ".font.zh-CN", ".font.zh-TW",
                                                                  ".font.ja-JP", ".font.ko-KR"};
constexpr bool IsSystemFontFile(std::string_view name) {
    for (const auto reserved : kSystemFontFiles)
        if (name == reserved) return true;
    return false;
}
}  // namespace micropixel::runtime
