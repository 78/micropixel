// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>

#include "lvgl.h"
#include "platform/memory/psram_buffer.hpp"

namespace micropixel::platform::lvgl {

// Fixed-charset Tiny TTF accelerator. The source and its size/kerning configuration
// must remain alive and unchanged until Reset(). All access, including teardown,
// requires the same LVGL adapter lock. Initialize before publishing the font.
// Source glyph/bitmap caches must fit the charset; configure Tiny TTF's own
// kerning cache to zero so pair misses compute without allocating during draw.
class TinyTtfFontCache final {
   public:
    enum class Error { kInvalidArgument, kAlreadyInitialized, kNoMemory, kGlyphUnavailable };

    TinyTtfFontCache() = default;
    TinyTtfFontCache(const TinyTtfFontCache&) = delete;
    TinyTtfFontCache& operator=(const TinyTtfFontCache&) = delete;
    ~TinyTtfFontCache() { Reset(); }

    // Sorted, unique printable Unicode scalars; unprepared characters fall back.
    // Pair capacity is a power of two, at least four; storage never grows.
    [[nodiscard]] std::expected<void, Error> Initialize(lv_font_t& source, std::span<const uint32_t> charset,
                                                        size_t pair_capacity = 1024U);
    void Reset();
    [[nodiscard]] const lv_font_t* font() const { return source_ == nullptr ? nullptr : &font_; }

   private:
    struct Glyph {
        uint32_t codepoint{};
        lv_font_glyph_dsc_t descriptor{};
        const lv_draw_buf_t* bitmap{};
    };
    struct Pair {
        uint32_t key{};
        uint16_t advance{};
    };
    static bool Descriptor(const lv_font_t* font, lv_font_glyph_dsc_t* glyph, uint32_t codepoint, uint32_t next);
    static const void* Bitmap(lv_font_glyph_dsc_t* glyph, lv_draw_buf_t*);
    static void Release(const lv_font_t*, lv_font_glyph_dsc_t*) {}
    [[nodiscard]] uint16_t Find(uint32_t codepoint) const;
    [[nodiscard]] uint16_t Advance(uint16_t first, uint16_t next);

    lv_font_t font_{};
    lv_font_t* source_{};
    memory::PsramBuffer<Glyph> glyphs_{};
    memory::PsramBuffer<Pair> pairs_{};
    std::array<uint16_t, 256> latin_{};
    uint32_t replacement_{};
};

}  // namespace micropixel::platform::lvgl
