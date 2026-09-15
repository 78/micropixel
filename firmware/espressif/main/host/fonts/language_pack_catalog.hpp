// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
namespace micropixel::host::fonts {
struct Pack {
    const char* locale;
};
// Local language menu; availability and artifact URLs come from the Control API.
inline constexpr std::array<Pack, 5> kPacks{{{"en"}, {"zh-CN"}, {"zh-TW"}, {"ja-JP"}, {"ko-KR"}}};
}  // namespace micropixel::host::fonts
