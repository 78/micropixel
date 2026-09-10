#pragma once

#include "platform/graphics/app_surface_compositor.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"

namespace micropixel::platform::lvgl {

// Reads the validated, uncompressed bitmap-font descriptors already owned by
// FontRegistry and paints glyph coverage directly into the App Surface. No
// LVGL objects, draw tasks, layers or transient allocations are involved.
class BitmapFontRasterizer final : public graphics::TextRasterizer {
   public:
    explicit BitmapFontRasterizer(FontRegistry& fonts) : fonts_(fonts) {}

    [[nodiscard]] bool Measure(micropixel_font_handle_t font_handle, const char* text, uint16_t text_length,
                               graphics::RasterTextMetrics& metrics) const override;
    [[nodiscard]] bool Draw(graphics::PixelSurface destination, int32_t x, int32_t y, uint32_t rgb888,
                            micropixel_font_handle_t font_handle, const char* text,
                            uint16_t text_length) const override;
    // Same painter for a font the caller already resolved (raster TEXT
    // records name Guest handles rather than retained Scene handles).
    [[nodiscard]] bool DrawWithFont(graphics::PixelSurface destination, int32_t x, int32_t y, uint32_t rgb888,
                                    const lv_font_t* font, const char* text, uint16_t text_length) const;

   private:
    FontRegistry& fonts_;
};

}  // namespace micropixel::platform::lvgl
