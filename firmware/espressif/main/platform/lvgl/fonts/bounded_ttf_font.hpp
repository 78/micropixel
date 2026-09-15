// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <span>

#include "lvgl.h"

namespace micropixel::platform::lvgl {

// Tiny TTF's stb rasterizer with fixed glyph slots and a preallocated scratch
// arena. All calls require the LVGL adapter lock. Input bytes outlive the font;
// only firmware-pinned, SHA-256 verified static TrueType packs are accepted.
class BoundedTtfFont final {
   public:
    BoundedTtfFont();
    ~BoundedTtfFont();
    BoundedTtfFont(const BoundedTtfFont&) = delete;
    BoundedTtfFont& operator=(const BoundedTtfFont&) = delete;
    [[nodiscard]] bool Initialize(std::span<const uint8_t> bytes, uint32_t size);
    void Reset();
    [[nodiscard]] const lv_font_t* font() const;

   private:
    struct State;
    struct Deleter {
        void operator()(State*) const;
    };
    static bool Descriptor(const lv_font_t*, lv_font_glyph_dsc_t*, uint32_t, uint32_t);
    static const void* Bitmap(lv_font_glyph_dsc_t*, lv_draw_buf_t*);
    static void Release(const lv_font_t*, lv_font_glyph_dsc_t*);
    std::unique_ptr<State, Deleter> state_{};
};

}  // namespace micropixel::platform::lvgl
