// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>

namespace micropixel::platform::lvgl {

struct DialogSnapshotLayout final {
    uint32_t stride_bytes{};
    uint32_t pitch_pixels{};
    uint32_t buffer_bytes{};
};

// LVGL may pad ARGB8888 rows. PPA's picture width expresses that row pitch,
// while the visible rectangle retains the actual dialog width.
[[nodiscard]] constexpr std::optional<DialogSnapshotLayout> PlanDialogSnapshot(uint32_t width, uint32_t height,
                                                                               uint32_t stride_bytes) {
    if (width == 0U || height == 0U || width > UINT32_MAX / 4U || stride_bytes < width * 4U ||
        stride_bytes % 4U != 0U || stride_bytes > UINT32_MAX / height) {
        return std::nullopt;
    }
    return DialogSnapshotLayout{stride_bytes, stride_bytes / 4U, stride_bytes * height};
}

}  // namespace micropixel::platform::lvgl
