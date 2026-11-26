// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

#include "platform/graphics/pixel_compositor.hpp"

namespace mpx_port {
struct DesktopItem {
    const char* id;
    const char* name;
    const char* version;
    uint32_t bytes;
    bool on_sd{};
};
constexpr uint32_t kDesktopPageSize = 6;
constexpr int kDesktopHome = -2;
constexpr int kDesktopPrevious = -3;
constexpr int kDesktopNext = -4;
// Returns a visible slot or a navigation action; -1 means no target.
int DesktopHit(int32_t x, int32_t y, uint32_t width, uint32_t height);
bool DesktopHomeHit(int32_t x, int32_t y, uint32_t width);
void SetDesktopGuestOverlay(bool enabled);
void DrawDesktopGuestOverlay(micropixel::platform::graphics::PixelSurface destination);
}  // namespace mpx_port
