#include <cassert>

#include "abi/micropixel_abi.h"
#include "platform/metalio-claw4/fonts/font_registry.hpp"

namespace {

bool GlyphDescriptor(const lv_font_t*, lv_font_glyph_dsc_t*, uint32_t, uint32_t) { return true; }

const void* GlyphBitmap(lv_font_glyph_dsc_t*, lv_draw_buf_t*) { return nullptr; }

const lv_font_t kCandidateSmall{.get_glyph_dsc = GlyphDescriptor, .get_glyph_bitmap = GlyphBitmap, .line_height = 15};
const lv_font_t kCandidateMedium{.get_glyph_dsc = GlyphDescriptor, .get_glyph_bitmap = GlyphBitmap, .line_height = 19};
const lv_font_t kCandidateLarge{.get_glyph_dsc = GlyphDescriptor, .get_glyph_bitmap = GlyphBitmap, .line_height = 25};
const lv_font_t kCandidateTitle{.get_glyph_dsc = GlyphDescriptor, .get_glyph_bitmap = GlyphBitmap, .line_height = 33};
const lv_font_t kInvalidFont{.get_glyph_bitmap = GlyphBitmap, .line_height = 15};

}  // namespace

extern "C" {
extern const lv_font_t font_builtin_latin_14{
    .get_glyph_dsc = GlyphDescriptor, .get_glyph_bitmap = GlyphBitmap, .line_height = 18};
extern const lv_font_t font_builtin_latin_18{
    .get_glyph_dsc = GlyphDescriptor, .get_glyph_bitmap = GlyphBitmap, .line_height = 23};
extern const lv_font_t font_builtin_latin_24{
    .get_glyph_dsc = GlyphDescriptor, .get_glyph_bitmap = GlyphBitmap, .line_height = 30};
extern const lv_font_t font_builtin_latin_32{
    .get_glyph_dsc = GlyphDescriptor, .get_glyph_bitmap = GlyphBitmap, .line_height = 39};
}

bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t* font, lv_font_glyph_dsc_t* descriptor, uint32_t codepoint,
                                   uint32_t next_codepoint) {
    return GlyphDescriptor(font, descriptor, codepoint, next_codepoint);
}

const void* lv_font_get_bitmap_fmt_txt(lv_font_glyph_dsc_t* descriptor, lv_draw_buf_t* draw_buffer) {
    return GlyphBitmap(descriptor, draw_buffer);
}

int main() {
    using micropixel::platform::metalio_claw4::FontRegistry;
    using micropixel::platform::metalio_claw4::SystemFontSet;

    FontRegistry registry;
    assert(registry.ResolveSystemHandle(MICROPIXEL_SYSTEM_FONT_SMALL) == &font_builtin_latin_14);
    assert(registry.ResolveSystemHandle(MICROPIXEL_SYSTEM_FONT_MEDIUM) == &font_builtin_latin_18);
    assert(registry.ResolveSystemHandle(MICROPIXEL_SYSTEM_FONT_LARGE) == &font_builtin_latin_24);
    assert(registry.ResolveSystemHandle(MICROPIXEL_SYSTEM_FONT_TITLE) == &font_builtin_latin_32);
    assert(registry.ResolveSystemHandle(0U) == nullptr);
    assert(registry.ResolveSystemHandle(5U) == nullptr);

    const uint32_t builtin_generation = registry.generation();
    const SystemFontSet candidate{.fonts = {
                                      &kCandidateSmall,
                                      &kCandidateMedium,
                                      &kCandidateLarge,
                                      &kCandidateTitle,
                                  }};
    assert(registry.Activate(candidate));
    assert(registry.generation() == builtin_generation + 1U);
    assert(registry.ResolveSystemHandle(MICROPIXEL_SYSTEM_FONT_TITLE) == &kCandidateTitle);

    SystemFontSet invalid = candidate;
    invalid.fonts[0] = &kInvalidFont;
    assert(!registry.Activate(invalid));
    assert(registry.ResolveSystemHandle(MICROPIXEL_SYSTEM_FONT_SMALL) == &kCandidateSmall);

    registry.ResetToBuiltin();
    assert(registry.ResolveSystemHandle(MICROPIXEL_SYSTEM_FONT_SMALL) == &font_builtin_latin_14);
    return 0;
}
