// SPDX-License-Identifier: Apache-2.0
#include "desktop.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "graphics.hpp"
#include "media_display.h"
#include "mpx_board.h"

namespace mpx_port {
namespace {
namespace g = micropixel::platform::graphics;
std::atomic<bool> guest_overlay{};
constexpr uint32_t colors[] = {0x8067d8, 0x268faa, 0x299c7c, 0xc77c3e, 0xb45d9d, 0x567fc7};
struct Grid {
    int32_t x, y, w, h, gap;
};
Grid Layout(uint32_t width, uint32_t height) {
    if (width < 600)
        return {12, 104, (static_cast<int32_t>(width) - 36) / 2, (static_cast<int32_t>(height) - 176) / 3, 12};
    return {40, 150, (static_cast<int32_t>(width) - 120) / 3, (static_cast<int32_t>(height) - 250) / 2, 20};
}
bool Inside(int32_t x, int32_t y, int32_t left, int32_t top, int32_t w, int32_t h) {
    return x >= left && y >= top && x < left + w && y < top + h;
}
}  // namespace
bool DesktopHomeHit(int32_t x, int32_t y, uint32_t width) {
    return Inside(x, y, static_cast<int32_t>(width) - 108, 12, 96, 44);
}
int DesktopHit(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    const auto grid = Layout(width, height);
    for (int slot = 0; slot < 6; ++slot) {
        if (Inside(x, y, grid.x + slot % (width < 600 ? 2 : 3) * (grid.w + grid.gap),
                   grid.y + slot / (width < 600 ? 2 : 3) * (grid.h + grid.gap), grid.w, grid.h))
            return slot;
    }
    if (Inside(x, y, 40, height - 58, 110, 42)) return kDesktopPrevious;
    if (Inside(x, y, width - 150, height - 58, 110, 42)) return kDesktopNext;
    return -1;
}
void SetDesktopGuestOverlay(bool enabled) { guest_overlay.store(enabled); }
void DrawDesktopGuestOverlay(g::PixelSurface destination) {
    if (!guest_overlay.load()) return;
    g::SoftwarePixelCompositor pixels;
    const int32_t x = static_cast<int32_t>(destination.width) - 108;
    (void)pixels.RoundedRect(destination, {x, 12, 96, 44}, 12, 0x152a3a, 0x65ddc1, 2, 255);
    // A host-owned house glyph is visible even when a Guest fills every pixel.
    (void)pixels.Fill(destination, {x + 36, 30, 24, 16}, 0x65ddc1, 255);
    for (int32_t row = 0; row < 12; ++row)
        (void)pixels.Fill(destination, {x + 48 - row, 18 + row, 1 + 2 * row, 1}, 0x65ddc1, 255);
    (void)pixels.Fill(destination, {x + 45, 35, 7, 11}, 0x152a3a, 255);
}

bool Graphics::ShowDesktop(const DesktopItem* items, uint32_t count, uint32_t page, uint32_t total, uint64_t used,
                           uint64_t capacity, const char* notice) {
    std::lock_guard lock(mutex_);
    uint32_t pitch{};
    auto* buffer = media_display_acquire_direct_rgb565(&pitch);
    if (!buffer) return false;
    g::PixelSurface dest{reinterpret_cast<uint8_t*>(buffer), Width() * Height() * 2, Width(), Height(), pitch * 2,
                         g::SurfacePixelFormat::kRgb565};
    auto fill = [&](g::SurfaceRect r, uint32_t c) { (void)pixels_.Fill(dest, r, c, 255); };
    auto round = [&](g::SurfaceRect r, uint32_t c, uint32_t radius) {
        (void)pixels_.RoundedRect(dest, r, radius, c, 0, 0, 255);
    };
    auto text = [&](int32_t x, int32_t y, const char* value, uint32_t color, uint32_t font) {
        (void)text_.Draw(dest, x, y, color, font, value, std::strlen(value));
    };
    fill({0, 0, static_cast<int32_t>(Width()), static_cast<int32_t>(Height())}, 0x101a2b);
    const bool compact = Width() < 600;
    if (compact) {
        text(12, 10, "MicroPixel", 0xf3f6fc, MICROPIXEL_SYSTEM_FONT_TITLE);
        text(12, 49, "" MPX_BOARD_NAME " / SD", 0x65ddc1, MICROPIXEL_SYSTEM_FONT_SMALL);
    } else {
        text(40, 30, "MicroPixel / " MPX_BOARD_NAME "", 0xf3f6fc, MICROPIXEL_SYSTEM_FONT_TITLE);
    }
    const auto grid = Layout(Width(), Height());
    for (uint32_t i = 0; i < count && i < 6; ++i) {
        int32_t x = grid.x + (i % (compact ? 2 : 3)) * (grid.w + grid.gap),
                y = grid.y + (i / (compact ? 2 : 3)) * (grid.h + grid.gap);
        round({x, y, grid.w, grid.h}, 0x1c2c43, 18);
        const uint32_t color = colors[(page * 6 + i) % 6];
        round({x + 8, y + 8, 6, grid.h - 16}, color, 3);
        const char* title = items[i].name ? items[i].name : "App";
        size_t length = std::min<size_t>(compact ? 12 : 20, std::strlen(title));
        while (length && (static_cast<uint8_t>(title[length]) & 0xc0) == 0x80) --length;
        char name[24]{};
        std::memcpy(name, title, length);
        text(x + 20, y + 12, name, 0xf1f5fb, MICROPIXEL_SYSTEM_FONT_SMALL);
        char detail[64];
        std::snprintf(detail, sizeof(detail), "%lu KiB / %s",
                      static_cast<unsigned long>((items[i].bytes + 1023) / 1024), items[i].on_sd ? "SD" : "FLASH");
        text(x + 20, y + 46, detail, 0x9caec8, MICROPIXEL_SYSTEM_FONT_SMALL);
    }
    if (!total) text(12, 160, "Install a Bundle via UART.", 0x9caec8, MICROPIXEL_SYSTEM_FONT_MEDIUM);
    const uint32_t pages = total ? (total + 5) / 6 : 1;
    char status[160];
    std::snprintf(status, sizeof(status), "%lu apps   /   %llu / %llu KiB   /   page %lu of %lu",
                  static_cast<unsigned long>(total), static_cast<unsigned long long>(used / 1024),
                  static_cast<unsigned long long>(capacity / 1024), static_cast<unsigned long>(page + 1),
                  static_cast<unsigned long>(pages));
    text(12, Height() - 24, status, 0x9caec8, MICROPIXEL_SYSTEM_FONT_SMALL);
    if (page) {
        round({40, static_cast<int32_t>(Height()) - 58, 110, 42}, 0x29425b, 12);
        text(55, Height() - 48, "PREVIOUS", 0xf3f6fc, MICROPIXEL_SYSTEM_FONT_SMALL);
    }
    if (page + 1 < pages) {
        round({static_cast<int32_t>(Width()) - 150, static_cast<int32_t>(Height()) - 58, 110, 42}, 0x29425b, 12);
        text(Width() - 121, Height() - 48, "NEXT", 0xf3f6fc, MICROPIXEL_SYSTEM_FONT_SMALL);
    }
    if (notice && *notice)
        text(12, 76, compact ? "Tap an app to launch" : notice, 0x65ddc1, MICROPIXEL_SYSTEM_FONT_SMALL);
    return media_display_present_direct_rgb565(buffer, 0, 0, Width(), Height());
}
}  // namespace mpx_port
