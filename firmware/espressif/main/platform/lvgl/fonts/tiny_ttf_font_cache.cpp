// SPDX-License-Identifier: Apache-2.0
#include "platform/lvgl/fonts/tiny_ttf_font_cache.hpp"

#include <algorithm>

namespace micropixel::platform::lvgl {

std::expected<void, TinyTtfFontCache::Error> TinyTtfFontCache::Initialize(lv_font_t& source,
                                                                          std::span<const uint32_t> charset,
                                                                          size_t pair_capacity) {
    if (source_ != nullptr) return std::unexpected(Error::kAlreadyInitialized);
    if (charset.empty() || charset.size() > UINT16_MAX || pair_capacity < 4U ||
        (pair_capacity & (pair_capacity - 1U)) != 0U || source.get_glyph_dsc == nullptr ||
        source.get_glyph_bitmap == nullptr || source.release_glyph == nullptr) {
        return std::unexpected(Error::kInvalidArgument);
    }
    uint32_t previous = 0U;
    for (const uint32_t codepoint : charset) {
        if (codepoint < 32U || codepoint <= previous || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return std::unexpected(Error::kInvalidArgument);
        }
        previous = codepoint;
    }
    if (!glyphs_.Allocate(charset.size()) || !pairs_.Allocate(pair_capacity)) {
        Reset();
        return std::unexpected(Error::kNoMemory);
    }
    source_ = &source;
    for (size_t i = 0U; i < charset.size(); ++i) {
        auto& record = glyphs_.View()[i];
        record.codepoint = charset[i];
        auto& glyph = record.descriptor;
        if (!source.get_glyph_dsc(&source, &glyph, charset[i], 0U) || glyph.is_placeholder ||
            (glyph.box_w != 0U && glyph.box_h != 0U && glyph.format != LV_FONT_GLYPH_FORMAT_A8)) {
            Reset();
            return std::unexpected(Error::kGlyphUnavailable);
        }
        glyph.resolved_font = &source;
        if (glyph.box_w != 0U && glyph.box_h != 0U) {
            // Keep the source cache reference pinned until Reset; no per-draw
            // acquire/release, bitmap duplication, or rasterization is needed.
            record.bitmap = static_cast<const lv_draw_buf_t*>(source.get_glyph_bitmap(&glyph, nullptr));
            if (record.bitmap == nullptr || record.bitmap->data == nullptr) {
                Reset();
                return std::unexpected(Error::kGlyphUnavailable);
            }
            glyph.stride = record.bitmap->header.stride;
        }
        if (charset[i] < latin_.size()) latin_[charset[i]] = static_cast<uint16_t>(i + 1U);
    }
    font_ = source;
    font_.dsc = this;
    font_.get_glyph_dsc = Descriptor;
    font_.get_glyph_bitmap = Bitmap;
    font_.release_glyph = Release;
    return {};
}

void TinyTtfFontCache::Reset() {
    if (source_ != nullptr) {
        for (auto& record : glyphs_.View()) {
            if (record.descriptor.entry != nullptr) source_->release_glyph(source_, &record.descriptor);
        }
    }
    source_ = nullptr;
    glyphs_.Reset();
    pairs_.Reset();
    latin_.fill(0U);
    replacement_ = 0U;
    font_ = {};
}

uint16_t TinyTtfFontCache::Find(uint32_t codepoint) const {
    if (codepoint < latin_.size()) return latin_[codepoint];
    const auto glyphs = glyphs_.View();
    const auto found = std::lower_bound(glyphs.begin(), glyphs.end(), codepoint,
                                        [](const Glyph& glyph, uint32_t value) { return glyph.codepoint < value; });
    return found != glyphs.end() && found->codepoint == codepoint ? static_cast<uint16_t>(found - glyphs.begin() + 1U)
                                                                  : 0U;
}

uint16_t TinyTtfFontCache::Advance(uint16_t first, uint16_t next) {
    const auto& record = glyphs_.View()[first - 1U];
    if (next == 0U || source_->kerning == LV_FONT_KERNING_NONE) return record.descriptor.adv_w;
    const uint32_t key = (static_cast<uint32_t>(first) << 16U) | next;
    uint32_t hash = key ^ (key >> 13U);
    hash *= 0x85ebca6bU;
    hash ^= hash >> 16U;
    const size_t bucket = (hash & (pairs_.size() / 4U - 1U)) * 4U;
    auto entries = pairs_.View().subspan(bucket, 4U);
    Pair* empty = nullptr;
    for (auto& entry : entries) {
        if (entry.key == key) return entry.advance;
        if (entry.key == 0U) empty = &entry;
    }
    lv_font_glyph_dsc_t measured{};
    if (!source_->get_glyph_dsc(source_, &measured, record.codepoint, glyphs_.View()[next - 1U].codepoint)) {
        return record.descriptor.adv_w;
    }
    auto& target = empty != nullptr ? *empty : entries[replacement_++ & 3U];
    target = {.key = key, .advance = measured.adv_w};
    return target.advance;
}

bool TinyTtfFontCache::Descriptor(const lv_font_t* font, lv_font_glyph_dsc_t* glyph, uint32_t codepoint,
                                  uint32_t next) {
    auto& cache = *static_cast<TinyTtfFontCache*>(const_cast<void*>(font->dsc));
    if (codepoint < 32U) return cache.source_->get_glyph_dsc(cache.source_, glyph, codepoint, 0U);
    const uint16_t index = cache.Find(codepoint);
    if (index == 0U) return false;
    *glyph = cache.glyphs_.View()[index - 1U].descriptor;
    glyph->adv_w = cache.Advance(index, cache.Find(next));
    glyph->resolved_font = font;
    glyph->gid.index = index - 1U;
    glyph->entry = nullptr;
    return true;
}

const void* TinyTtfFontCache::Bitmap(lv_font_glyph_dsc_t* glyph, lv_draw_buf_t*) {
    const auto& cache = *static_cast<const TinyTtfFontCache*>(glyph->resolved_font->dsc);
    const auto* bitmap = cache.glyphs_.View()[glyph->gid.index].bitmap;
    if (bitmap == nullptr) return nullptr;
    return glyph->req_raw_bitmap ? static_cast<const void*>(bitmap->data) : bitmap;
}

}  // namespace micropixel::platform::lvgl
