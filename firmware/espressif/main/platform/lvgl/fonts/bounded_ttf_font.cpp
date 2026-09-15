// SPDX-License-Identifier: Apache-2.0
#include "platform/lvgl/fonts/bounded_ttf_font.hpp"

#include <array>
#include <cmath>
#include <cstring>

#include "platform/memory/ext_ram_bss.hpp"
#include "platform/memory/psram_buffer.hpp"

namespace {
struct Scratch {
    micropixel::platform::memory::PsramBuffer<uint8_t> bytes;
    size_t used{};
    bool failed{};
    void* Allocate(size_t size) {
        const size_t aligned = (size + 15U) & ~size_t{15U};
        if (aligned < size || aligned > bytes.size() - used) {
            failed = true;
            return nullptr;
        }
        auto* result = bytes.View().data() + used;
        used += aligned;
        return result;
    }
};
// Shared across the four font sizes, serialized by the adapter lock.
static MICROPIXEL_EXT_RAM_BSS Scratch scratch;
void* TtfAllocate(size_t bytes, void*) { return scratch.Allocate(bytes); }
}  // namespace

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_HEAP_FACTOR_SIZE_32 50
#define STBTT_HEAP_FACTOR_SIZE_128 20
#define STBTT_HEAP_FACTOR_SIZE_DEFAULT 10
#define STBTT_malloc(x, u) TtfAllocate(x, u)
#define STBTT_free(x, u) ((void)(x), (void)(u))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "src/libs/tiny_ttf/stb_truetype_htcw.h"
#pragma GCC diagnostic pop

namespace micropixel::platform::lvgl {
namespace {
constexpr size_t kGlyphSlots = 128U;
constexpr size_t kMetricsSlots = 512U;
constexpr size_t kScratchBytes = 512U * 1024U;
constexpr size_t kAlignment = 128U;
uint32_t Read32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24U) | (uint32_t{p[1]} << 16U) | (uint32_t{p[2]} << 8U) | p[3];
}
// This is a format check, not an untrusted-font sandbox. Authenticity is checked
// against the firmware manifest before the stb parser sees these bytes.
bool StaticTrueType(std::span<const uint8_t> bytes) {
    if (bytes.size() < 12U || Read32(bytes.data()) != 0x00010000U) return false;
    const size_t count = (uint32_t{bytes[4]} << 8U) | bytes[5];
    if (count > (bytes.size() - 12U) / 16U) return false;
    bool glyf = false;
    for (size_t i = 0U; i < count; ++i) {
        const auto* table = bytes.data() + 12U + i * 16U;
        const uint32_t offset = Read32(table + 8U), length = Read32(table + 12U);
        if (offset > bytes.size() || length > bytes.size() - offset) return false;
        if (Read32(table) == 0x66766172U) return false;  // fvar
        glyf |= Read32(table) == 0x676c7966U;
    }
    return glyf;
}
}  // namespace

struct BoundedTtfFont::State {
    struct Metric {
        uint32_t codepoint{};
        lv_font_glyph_dsc_t glyph{};
    };
    struct BitmapSlot {
        uint32_t glyph{};
        uint32_t used{};
        uint16_t pins{};
        lv_draw_buf_t buffer{};
    };
    lv_font_t font{};
    stbtt_fontinfo info{};
    float scale{};
    uint32_t side{};
    uint32_t slot_bytes{};
    uint32_t sequence{};
    memory::PsramBuffer<uint8_t> pixels{};
    std::array<Metric, kMetricsSlots> metrics{};
    std::array<BitmapSlot, kGlyphSlots> bitmaps{};
};

BoundedTtfFont::BoundedTtfFont() = default;
BoundedTtfFont::~BoundedTtfFont() = default;
void BoundedTtfFont::Deleter::operator()(State* state) const {
    if (state) {
        std::destroy_at(state);
        heap_caps_free(state);
    }
}
void BoundedTtfFont::Reset() { state_.reset(); }
const lv_font_t* BoundedTtfFont::font() const { return state_ ? &state_->font : nullptr; }

bool BoundedTtfFont::Initialize(std::span<const uint8_t> bytes, uint32_t size) {
    if (state_ || size < 8U || size > 32U || !StaticTrueType(bytes)) return false;
    std::unique_ptr<State, Deleter> candidate;
    auto* storage = static_cast<State*>(heap_caps_malloc(sizeof(State), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!storage) return false;
    candidate.reset(std::construct_at(storage));
    if (scratch.bytes.size() == 0U && !scratch.bytes.Allocate(kScratchBytes)) return false;
    auto& state = *candidate;
    if (!stbtt_InitFont(&state.info, bytes.data(), 0)) return false;
    state.scale = stbtt_ScaleForMappingEmToPixels(&state.info, static_cast<float>(size));
    state.side = (size * 2U + 15U) & ~15U;
    state.slot_bytes = state.side * state.side;
    if (!state.pixels.Allocate(state.slot_bytes * kGlyphSlots + kAlignment)) return false;
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&state.info, &ascent, &descent, &gap);
    state.font.line_height = static_cast<uint16_t>(std::round((ascent - descent + gap) * state.scale));
    state.font.base_line = static_cast<int16_t>(std::round(-descent * state.scale));
    state.font.get_glyph_dsc = Descriptor;
    state.font.get_glyph_bitmap = Bitmap;
    state.font.release_glyph = Release;
    state.font.dsc = &state;
    // CJK packs have no Latin responsibility: the prepared Latin font precedes
    // this fallback. CJK advances are fixed; skip a potentially large GPOS scan.
    state.font.kerning = LV_FONT_KERNING_NONE;
    state_ = std::move(candidate);
    return true;
}

bool BoundedTtfFont::Descriptor(const lv_font_t* font, lv_font_glyph_dsc_t* out, uint32_t cp, uint32_t) {
    if (cp < 32U || cp > 0x10FFFFU) return false;
    auto& state = *const_cast<State*>(static_cast<const State*>(font->dsc));
    auto& metric = state.metrics[(cp * 2654435761U) % kMetricsSlots];
    if (metric.codepoint != cp) {
        const int glyph = stbtt_FindGlyphIndex(&state.info, static_cast<int>(cp));
        if (!glyph) return false;
        int x1, y1, x2, y2, advance, bearing;
        stbtt_GetGlyphBitmapBox(&state.info, glyph, state.scale, state.scale, &x1, &y1, &x2, &y2);
        stbtt_GetGlyphHMetrics(&state.info, glyph, &advance, &bearing);
        const int width = x2 - x1 + 1, height = y2 - y1 + 1;
        if (width < 0 || height < 0 || width > static_cast<int>(state.side) || height > static_cast<int>(state.side))
            return false;
        metric.glyph = {};
        metric.glyph.adv_w = static_cast<uint16_t>(state.scale * advance + 0.5f);
        metric.glyph.box_w = width;
        metric.glyph.box_h = height;
        metric.glyph.ofs_x = x1;
        metric.glyph.ofs_y = -y2;
        metric.glyph.stride = state.side;
        metric.glyph.gid.index = glyph;
        metric.glyph.format = LV_FONT_GLYPH_FORMAT_A8;
        metric.codepoint = cp;
    }
    *out = metric.glyph;
    return true;
}

const void* BoundedTtfFont::Bitmap(lv_font_glyph_dsc_t* glyph, lv_draw_buf_t*) {
    auto& state = *const_cast<State*>(static_cast<const State*>(glyph->resolved_font->dsc));
    State::BitmapSlot* slot = nullptr;
    for (auto& entry : state.bitmaps) {
        if (entry.glyph == glyph->gid.index) {
            slot = &entry;
            break;
        }
        if (!entry.pins && (!slot || entry.used < slot->used)) slot = &entry;
    }
    if (!slot || slot->pins == UINT16_MAX) return nullptr;
    if (slot->glyph != glyph->gid.index) {
        if (slot->pins) return nullptr;
        const size_t index = slot - state.bitmaps.data();
        auto address = reinterpret_cast<uintptr_t>(state.pixels.View().data());
        address = (address + kAlignment - 1U) & ~(kAlignment - 1U);
        auto* pixels = reinterpret_cast<uint8_t*>(address) + index * state.slot_bytes;
        if (lv_draw_buf_init(&slot->buffer, glyph->box_w, glyph->box_h, LV_COLOR_FORMAT_A8, state.side, pixels,
                             state.slot_bytes) != LV_RESULT_OK)
            return nullptr;
        std::memset(pixels, 0, state.slot_bytes);
        scratch.used = 0U;
        scratch.failed = false;
        stbtt_MakeGlyphBitmap(&state.info, pixels, glyph->box_w, glyph->box_h, state.side, state.scale, state.scale,
                              glyph->gid.index);
        if (scratch.failed) {
            slot->glyph = 0U;
            return nullptr;
        }
        slot->glyph = glyph->gid.index;
        lv_draw_buf_flush_cache(&slot->buffer, nullptr);
    }
    ++slot->pins;
    slot->used = ++state.sequence;
    glyph->entry = reinterpret_cast<lv_cache_entry_t*>(slot);
    return glyph->req_raw_bitmap ? static_cast<const void*>(slot->buffer.data) : &slot->buffer;
}

void BoundedTtfFont::Release(const lv_font_t*, lv_font_glyph_dsc_t* glyph) {
    auto* slot = reinterpret_cast<State::BitmapSlot*>(glyph->entry);
    if (slot && slot->pins) --slot->pins;
    glyph->entry = nullptr;
}

}  // namespace micropixel::platform::lvgl
