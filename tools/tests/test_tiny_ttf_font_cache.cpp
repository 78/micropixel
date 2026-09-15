// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>

#include "platform/lvgl/fonts/tiny_ttf_font_cache.hpp"

namespace {
using Cache = micropixel::platform::lvgl::TinyTtfFontCache;
constexpr std::array<uint32_t, 5> kCharset{32, 65, 86, 103, 0x4e2d};
unsigned allocations{}, live_allocations{}, descriptors{}, bitmaps{}, releases{};
int allocation_budget = -1;
uint32_t fail_glyph{}, fail_bitmap{};
lv_cache_entry_t entry{};
uint8_t pixels[]{0, 127, 255, 0};
lv_draw_buf_t bitmap{{4}, pixels};

uint16_t Advance(uint32_t cp, uint32_t next) { return 12U + (cp + next) % 7U; }
bool Describe(const lv_font_t*, lv_font_glyph_dsc_t* glyph, uint32_t cp, uint32_t next) {
    ++descriptors;
    *glyph = {};
    if (cp == fail_glyph) return false;
    if (cp < 32U) return true;
    glyph->adv_w = Advance(cp, next);
    glyph->box_w = cp == 32U ? 0U : 3U;
    glyph->box_h = cp == 32U ? 0U : 1U;
    glyph->ofs_x = -1;
    glyph->ofs_y = 2;
    glyph->gid.index = cp;
    glyph->format = LV_FONT_GLYPH_FORMAT_A8;
    return true;
}
const void* GetBitmap(lv_font_glyph_dsc_t* glyph, lv_draw_buf_t*) {
    ++bitmaps;
    assert(glyph->req_raw_bitmap == 0U);
    if (glyph->gid.index == fail_bitmap) return nullptr;
    glyph->entry = &entry;
    ++entry.references;
    return &bitmap;
}
void Release(const lv_font_t*, lv_font_glyph_dsc_t* glyph) {
    assert(glyph->entry == &entry && entry.references > 0U);
    --entry.references;
    ++releases;
    glyph->entry = nullptr;
}
lv_font_t Source() {
    return {.get_glyph_dsc = Describe,
            .get_glyph_bitmap = GetBitmap,
            .release_glyph = Release,
            .line_height = 20,
            .base_line = 3,
            .kerning = LV_FONT_KERNING_NORMAL};
}
lv_font_glyph_dsc_t Lookup(const lv_font_t* font, uint32_t cp, uint32_t next) {
    lv_font_glyph_dsc_t glyph{};
    assert(font->get_glyph_dsc(font, &glyph, cp, next));
    assert(glyph.adv_w == Advance(cp, next));
    return glyph;
}
}  // namespace

void* micropixel_test_psram_allocate(size_t size) {
    if (allocation_budget == 0) return nullptr;
    if (allocation_budget > 0) --allocation_budget;
    ++allocations;
    ++live_allocations;
    return std::malloc(size);
}
void micropixel_test_psram_free(void* memory) {
    if (memory != nullptr) --live_allocations;
    std::free(memory);
}

int main() {
    auto source = Source();
    Cache cache;
    assert(!cache.Initialize(source, {}));
    assert(!cache.Initialize(source, kCharset, 3U));
    const std::array<uint32_t, 2> duplicate{65, 65}, unsorted{86, 65}, surrogate{65, 0xd800};
    assert(!cache.Initialize(source, duplicate));
    assert(!cache.Initialize(source, unsorted));
    assert(!cache.Initialize(source, surrogate));
    allocation_budget = 1;
    assert(cache.Initialize(source, kCharset).error() == Cache::Error::kNoMemory);
    assert(live_allocations == 0U && cache.font() == nullptr);
    allocation_budget = -1;
    for (bool fail_descriptor : {true, false}) {
        (fail_descriptor ? fail_glyph : fail_bitmap) = 103U;
        assert(cache.Initialize(source, kCharset).error() == Cache::Error::kGlyphUnavailable);
        assert(live_allocations == 0U && entry.references == 0U && cache.font() == nullptr);
        fail_glyph = fail_bitmap = 0U;
    }
    assert(cache.Initialize(source, kCharset, 4U));
    assert(entry.references == 4U);
    assert(cache.Initialize(source, kCharset).error() == Cache::Error::kAlreadyInitialized);
    // Copying the exposed LVGL font (e.g. to preserve system baselines) works.
    auto font = *cache.font();
    assert(font.line_height == 20 && font.base_line == 3);
    const auto allocation_count = allocations, bitmap_count = bitmaps, release_count = releases;
    allocation_budget = 0;
    auto glyph = Lookup(&font, 65U, 86U);
    const auto descriptor_count = descriptors;
    for (unsigned i = 0U; i < 100U; ++i) {
        glyph = Lookup(&font, 65U, 86U);
        assert(glyph.stride == 4U && glyph.ofs_x == -1 && glyph.ofs_y == 2);
        assert(font.get_glyph_bitmap(&glyph, nullptr) == &bitmap);
        glyph.req_raw_bitmap = 1U;
        assert(font.get_glyph_bitmap(&glyph, nullptr) == pixels);
        font.release_glyph(&font, &glyph);
    }
    assert(descriptors == descriptor_count && entry.references == 4U);
    // Force collisions and replacement; cached advances must never leak across pairs.
    for (unsigned repeat = 0; repeat < 3U; ++repeat) {
        for (auto cp : kCharset) {
            for (auto next : kCharset) Lookup(&font, cp, next);
            Lookup(&font, cp, 0U);
        }
    }
    assert(!font.get_glyph_dsc(&font, &glyph, 0x4e00U, 0U));
    assert(font.get_glyph_dsc(&font, &glyph, 65U, 0x4e00U));
    assert(glyph.adv_w == Advance(65U, 0U));
    assert(font.get_glyph_dsc(&font, &glyph, '\n', 0U) && glyph.adv_w == 0U);
    assert(allocations == allocation_count && bitmaps == bitmap_count && releases == release_count);
    cache.Reset();
    assert(cache.font() == nullptr && entry.references == 0U && live_allocations == 0U);
    cache.Reset();
    allocation_budget = -1;
    source.kerning = LV_FONT_KERNING_NONE;
    assert(cache.Initialize(source, kCharset));
    assert(cache.font()->get_glyph_dsc(cache.font(), &glyph, 65U, 86U));
    assert(glyph.adv_w == Advance(65U, 0U));
    cache.Reset();
    assert(live_allocations == 0U && entry.references == 0U);
    std::cout << "Tiny TTF font cache tests passed\n";
}
