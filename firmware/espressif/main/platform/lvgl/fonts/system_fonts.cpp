// SPDX-License-Identifier: Apache-2.0
#include "platform/lvgl/fonts/system_fonts.hpp"

#include <array>
#include <memory>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "platform/lvgl/fonts/bounded_ttf_font.hpp"
#include "platform/lvgl/fonts/system_font_fallback.hpp"
#include "platform/lvgl/fonts/tiny_ttf_font_cache.hpp"
#include "platform/memory/ext_ram_bss.hpp"

extern const uint8_t system_ttf_start[] asm("_binary_Montserrat_Medium_ttf_start");
extern const uint8_t system_ttf_end[] asm("_binary_Montserrat_Medium_ttf_end");
extern "C" {
MICROPIXEL_EXT_RAM_BSS lv_font_t micropixel_system_font_default;
}

namespace micropixel::platform::lvgl {
namespace {
#if CONFIG_MICROPIXEL_BOARD_ESP32_S3_BOX_3 || CONFIG_MICROPIXEL_BOARD_SZPI_ESP32S3 || \
    CONFIG_MICROPIXEL_BOARD_M5STACK_CORES3
constexpr std::array<int, 4> kSizes{10, 12, 14, 18};
#elif CONFIG_MICROPIXEL_BOARD_ESP_MOSAICO
constexpr std::array<int, 4> kSizes{14, 16, 20, 26};
#else
constexpr std::array<int, 4> kSizes{14, 18, 24, 32};
#endif

constexpr auto LatinCharset() {
    std::array<uint32_t, 191> result{};
    size_t i = 0U;
    for (uint32_t cp = 32U; cp <= 255U; ++cp) {
        if (cp <= 126U || cp >= 160U) result[i++] = cp;
    }
    return result;
}
constexpr auto kLatinCharset = LatinCharset();

struct TinyDeleter {
    void operator()(lv_font_t* font) const {
        if (font != nullptr) lv_tiny_ttf_destroy(font);
    }
};
struct FontSlot {
    std::unique_ptr<lv_font_t, TinyDeleter> source{};
    TinyTtfFontCache cache{};
    std::array<BoundedTtfFont, 2> language_fonts{};
    SystemFontFallback fallback{};
    lv_font_t language_proxy{};
    lv_font_t proxy{};
};
struct FontState {
    std::array<FontSlot, 4> slots{};
    bool ready{};
    bool prepared{};
    bool prepared_english{};
    uint8_t active_bank{};
};
static MICROPIXEL_EXT_RAM_BSS FontState state;

void Reset() {
    for (auto& slot : state.slots) {
        slot.cache.Reset();
        slot.source.reset();
        slot.proxy = {};
    }
    micropixel_system_font_default = {};
    state.ready = false;
}
}  // namespace

esp_err_t InitializeSystemFonts() {
    if (state.ready) return ESP_OK;
    for (size_t i = 0U; i < state.slots.size(); ++i) {
        const auto* fallback = BuiltinLatinFont(static_cast<SystemFontRole>(i));
        auto& slot = state.slots[i];
        slot.source.reset(lv_tiny_ttf_create_data_ex(system_ttf_start, system_ttf_end - system_ttf_start, kSizes[i],
                                                     LV_FONT_KERNING_NORMAL, 256U));
        if (!slot.source || !slot.cache.Initialize(*slot.source, kLatinCharset)) {
            Reset();
            return ESP_ERR_NO_MEM;
        }
        slot.proxy = *slot.cache.font();
        slot.fallback.Initialize(slot.proxy, fallback);
        slot.proxy.line_height = fallback->line_height;
        slot.proxy.base_line = fallback->base_line;
        slot.proxy.underline_position = fallback->underline_position;
        slot.proxy.underline_thickness = fallback->underline_thickness;
    }
    micropixel_system_font_default = state.slots[0].proxy;
    state.ready = true;
    ESP_LOGI("system_fonts", "Tiny TTF ready: four sizes, prepared Latin glyphs and bounded kerning caches");
    return ESP_OK;
}

bool PrepareSystemLanguageFont(std::span<const uint8_t> verified_bytes) {
    if (esp_lv_adapter_lock(-1) != ESP_OK) return false;
    const auto candidate = 1U - state.active_bank;
    state.prepared = false;
    state.prepared_english = verified_bytes.empty();
    bool success = InitializeSystemFonts() == ESP_OK;
    for (auto& slot : state.slots) slot.language_fonts[candidate].Reset();
    for (size_t i = 0U; success && !verified_bytes.empty() && i < state.slots.size(); ++i)
        success = state.slots[i].language_fonts[candidate].Initialize(verified_bytes, kSizes[i]);
    if (!success)
        for (auto& slot : state.slots) slot.language_fonts[candidate].Reset();
    state.prepared = success;
    esp_lv_adapter_unlock();
    return success;
}

void AbortSystemLanguageFont() {
    if (esp_lv_adapter_lock(-1) != ESP_OK) return;
    for (auto& slot : state.slots) slot.language_fonts[1U - state.active_bank].Reset();
    state.prepared = false;
    esp_lv_adapter_unlock();
}

bool CommitSystemLanguageFont(CommitLanguageSetting commit_setting, void* context) {
    if (esp_lv_adapter_lock(-1) != ESP_OK) return false;
    if (!state.prepared || (commit_setting && !commit_setting(context))) {
        esp_lv_adapter_unlock();
        return false;
    }
    const auto candidate = 1U - state.active_bank;
    for (size_t i = 0U; i < state.slots.size(); ++i) {
        auto& slot = state.slots[i];
        if (!state.prepared_english) {
            slot.language_proxy = *slot.language_fonts[candidate].font();
            slot.language_proxy.line_height = slot.proxy.line_height;
            slot.language_proxy.base_line = slot.proxy.base_line;
        }
        slot.fallback.Activate(slot.proxy, state.prepared_english ? nullptr : &slot.language_proxy);
        slot.language_fonts[state.active_bank].Reset();
    }
    micropixel_system_font_default = state.slots[0].proxy;
    state.active_bank = candidate;
    state.prepared = false;
    esp_lv_adapter_unlock();
    return true;
}

const lv_font_t* SystemFont(SystemFontRole role, const lv_font_t* fallback) {
    if (!state.ready) return fallback;
    const size_t i = static_cast<size_t>(role);
    return i == 0U || i >= state.slots.size() ? &micropixel_system_font_default : &state.slots[i].proxy;
}

}  // namespace micropixel::platform::lvgl
