// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

#include "platform/lvgl/fonts/bounded_ttf_font.hpp"

namespace {
size_t allocations{};
bool deny_allocations{};
}  // namespace
void* micropixel_test_psram_allocate(size_t size) {
    assert(!deny_allocations);
    ++allocations;
    return std::malloc(size);
}
void micropixel_test_psram_free(void* memory) { std::free(memory); }

void TestFont(const char* path, uint32_t size) {
    std::ifstream input(path, std::ios::binary);
    assert(input.good());
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input), {});
    micropixel::platform::lvgl::BoundedTtfFont font;
    assert(!font.Initialize({}, 20));
    assert(!font.Initialize(bytes, 100));
    assert(font.Initialize(bytes, size));
    const auto* lvfont = font.font();
    const size_t count = allocations;
    const auto capacity = font.GetStatistics().glyph_capacity;
    std::vector<lv_font_glyph_dsc_t> pinned(capacity);
    std::vector<const uint8_t*> addresses(capacity);
    std::vector<uint8_t> first_bitmap;
    size_t used = 0;
    deny_allocations = true;
    // Pin every slot. Loading another glyph must fail without evicting any
    // active bitmap. Then release a slot and prove the miss succeeds.
    uint32_t cp = 32;
    while (used < pinned.size() && cp < 0x10000) {
        auto& glyph = pinned[used];
        if (!lvfont->get_glyph_dsc(lvfont, &glyph, cp++, 0)) continue;
        bool duplicate = false;
        for (size_t i = 0; i < used; ++i) duplicate |= pinned[i].gid.index == glyph.gid.index;
        if (duplicate) continue;
        glyph.resolved_font = lvfont;
        glyph.req_raw_bitmap = true;
        const auto* bitmap = static_cast<const uint8_t*>(lvfont->get_glyph_bitmap(&glyph, nullptr));
        assert(bitmap);
        addresses[used++] = bitmap;
    }
    assert(used == pinned.size());
    // First bitmap is a whitespace glyph but its complete slot must stay intact.
    first_bitmap.assign(addresses[0], addresses[0] + pinned[0].stride * pinned[0].box_h);
    lv_font_glyph_dsc_t extra{};
    while (!lvfont->get_glyph_dsc(lvfont, &extra, cp++, 0)) assert(cp < 0x10000);
    extra.resolved_font = lvfont;
    extra.req_raw_bitmap = true;
    assert(!lvfont->get_glyph_bitmap(&extra, nullptr));
    assert(std::memcmp(first_bitmap.data(), addresses[0], first_bitmap.size()) == 0);
    lvfont->release_glyph(lvfont, &pinned[1]);
    assert(lvfont->get_glyph_bitmap(&extra, nullptr));
    lvfont->release_glyph(lvfont, &extra);
    for (auto& glyph : pinned) lvfont->release_glyph(lvfont, &glyph);
    // Rasterize every supported scalar in the fixture, exercising composites,
    // cold misses, bitmap eviction and direct-mapped metric collisions.
    size_t rendered = 0;
    uint64_t pixel_hash = 14695981039346656037ULL;
    for (uint32_t unicode = 32; unicode <= 0x10ffff; ++unicode) {
        lv_font_glyph_dsc_t glyph{};
        if (!lvfont->get_glyph_dsc(lvfont, &glyph, unicode, 0)) continue;
        glyph.resolved_font = lvfont;
        const auto* bitmap = static_cast<const lv_draw_buf_t*>(lvfont->get_glyph_bitmap(&glyph, nullptr));
        assert(bitmap && bitmap->data && bitmap->header.stride == glyph.stride);
        for (uint32_t y = 0; y < glyph.box_h; ++y) {
            for (uint32_t x = 0; x < glyph.box_w; ++x) {
                pixel_hash ^= bitmap->data[y * glyph.stride + x];
                pixel_hash *= 1099511628211ULL;
            }
        }
        lvfont->release_glyph(lvfont, &glyph);
        ++rendered;
    }
    assert(rendered > capacity && allocations == count);
    const auto stats = font.GetStatistics();
    assert(stats.scratch_failures == 0U && stats.bitmap_failures == 1U);
    assert(stats.peak_pins == capacity && stats.glyphs_used == capacity);
    assert(stats.evictions > 0 && stats.scratch_peak_bytes <= stats.scratch_capacity_bytes);
    std::cout << "FONTSTAT file=" << path << " size=" << size << " glyphs=" << rendered
              << " bitmap_capacity=" << stats.bitmap_capacity_bytes << " pixel_peak=" << stats.peak_pixel_bytes
              << " max_width=" << stats.max_width << " max_height=" << stats.max_height
              << " scratch_peak=" << stats.scratch_peak_bytes << " scratch_capacity=" << stats.scratch_capacity_bytes
              << " pixel_hash=" << pixel_hash << " metadata=" << stats.metadata_bytes << '\n';
    deny_allocations = false;
    font.Reset();
    std::cout << "Bounded Tiny TTF: " << rendered << " glyphs, no draw allocations, pinned eviction passed\n";
}

void TestSizes(const char* path) {
    const char* sizes = std::getenv("MICROPIXEL_TEST_FONT_SIZES");
    if (!sizes) {
        TestFont(path, 26U);
        return;
    }
    while (*sizes) {
        char* end{};
        const auto size = std::strtoul(sizes, &end, 10);
        assert(end != sizes && size >= 8U && size <= 32U);
        TestFont(path, static_cast<uint32_t>(size));
        sizes = *end == ',' ? end + 1 : end;
    }
}

int main() {
    if (const char* directory = std::getenv("MICROPIXEL_TEST_LANGUAGE_FONTS_DIR")) {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() == ".ttf") TestSizes(entry.path().c_str());
        }
    } else
        TestSizes(MICROPIXEL_TEST_FONT);
}
