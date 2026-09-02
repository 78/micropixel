#include "platform/lvgl/fonts/font_registry.hpp"

#include <cstddef>
#include <utility>

#include "abi/micropixel_abi.h"
#include "platform/lvgl/fonts/font_cbin_loader.hpp"

extern "C" {
extern const lv_font_t font_builtin_latin_10;
extern const lv_font_t font_builtin_latin_12;
extern const lv_font_t font_builtin_latin_14;
extern const lv_font_t font_builtin_latin_16;
extern const lv_font_t font_builtin_latin_18;
extern const lv_font_t font_builtin_latin_20;
extern const lv_font_t font_builtin_latin_24;
extern const lv_font_t font_builtin_latin_26;
extern const lv_font_t font_builtin_latin_32;
}

namespace micropixel::platform::lvgl {
namespace {

constexpr size_t RoleIndex(SystemFontRole role) { return static_cast<size_t>(role); }
constexpr uint16_t kDynamicHandleBit = 0x8000U;
constexpr uint16_t kDynamicSlotMask = 0x0007U;
constexpr uint16_t kDynamicGenerationMask = 0x0fffU;

uint16_t DynamicHandle(uint32_t index, uint16_t generation) {
    return static_cast<uint16_t>(kDynamicHandleBit | ((generation & kDynamicGenerationMask) << 3U) | index);
}

const lv_font_t* ProfileFont(SystemFontRole role) {
#if CONFIG_MICROPIXEL_BOARD_ESP32_S3_BOX_3 || CONFIG_MICROPIXEL_BOARD_SZPI_ESP32S3
    switch (role) {
        case SystemFontRole::kTitle:
            return &font_builtin_latin_18;
        case SystemFontRole::kLarge:
            return &font_builtin_latin_14;
        case SystemFontRole::kMedium:
            return &font_builtin_latin_12;
        case SystemFontRole::kSmall:
        case SystemFontRole::kCount:
        default:
            return &font_builtin_latin_10;
    }
#elif CONFIG_MICROPIXEL_BOARD_ESP_MOSAICO
    switch (role) {
        case SystemFontRole::kTitle:
            return &font_builtin_latin_26;
        case SystemFontRole::kLarge:
            return &font_builtin_latin_20;
        case SystemFontRole::kMedium:
            return &font_builtin_latin_16;
        case SystemFontRole::kSmall:
        case SystemFontRole::kCount:
        default:
            return &font_builtin_latin_14;
    }
#else
    switch (role) {
        case SystemFontRole::kTitle:
            return &font_builtin_latin_32;
        case SystemFontRole::kLarge:
            return &font_builtin_latin_24;
        case SystemFontRole::kMedium:
            return &font_builtin_latin_18;
        case SystemFontRole::kSmall:
        case SystemFontRole::kCount:
        default:
            return &font_builtin_latin_14;
    }
#endif
}

SystemFontSet BuiltinFontSet() {
    return SystemFontSet{.fonts = {ProfileFont(SystemFontRole::kSmall), ProfileFont(SystemFontRole::kMedium),
                                   ProfileFont(SystemFontRole::kLarge), ProfileFont(SystemFontRole::kTitle)}};
}

}  // namespace

const lv_font_t* BuiltinLatinFont(SystemFontRole role) { return ProfileFont(role); }

FontRegistry::FontRegistry() : active_(BuiltinFontSet()) {}

FontRegistry::~FontRegistry() = default;

const lv_font_t* FontRegistry::Resolve(SystemFontRole role) const {
    const size_t index = RoleIndex(role);
    return index < active_.fonts.size() ? active_.fonts[index] : active_.fonts[RoleIndex(SystemFontRole::kSmall)];
}

const lv_font_t* FontRegistry::ResolveSystemHandle(uint16_t handle) const {
    switch (handle) {
        case MICROPIXEL_SYSTEM_FONT_TITLE:
            return Resolve(SystemFontRole::kTitle);
        case MICROPIXEL_SYSTEM_FONT_LARGE:
            return Resolve(SystemFontRole::kLarge);
        case MICROPIXEL_SYSTEM_FONT_MEDIUM:
            return Resolve(SystemFontRole::kMedium);
        case MICROPIXEL_SYSTEM_FONT_SMALL:
            return Resolve(SystemFontRole::kSmall);
        default:
            return nullptr;
    }
}

FontRegistry::DynamicFontSlot* FontRegistry::FindDynamic(micropixel_font_handle_t handle) {
    return const_cast<DynamicFontSlot*>(std::as_const(*this).FindDynamic(handle));
}

const FontRegistry::DynamicFontSlot* FontRegistry::FindDynamic(micropixel_font_handle_t handle) const {
    if ((handle & kDynamicHandleBit) == 0U) {
        return nullptr;
    }
    const uint32_t index = handle & kDynamicSlotMask;
    const uint16_t generation = static_cast<uint16_t>((handle >> 3U) & kDynamicGenerationMask);
    if (index >= dynamic_.size()) {
        return nullptr;
    }
    const DynamicFontSlot& slot = dynamic_[index];
    return slot.loaded != nullptr && generation != 0U && slot.generation == generation ? &slot : nullptr;
}

const lv_font_t* FontRegistry::ResolveGuestHandle(micropixel_font_handle_t handle) const {
    if ((handle & kDynamicHandleBit) == 0U) {
        return ResolveSystemHandle(handle);
    }
    const DynamicFontSlot* slot = FindDynamic(handle);
    return slot != nullptr && slot->guest_owned ? slot->loaded->font() : nullptr;
}

const lv_font_t* FontRegistry::ResolveRetainedHandle(micropixel_font_handle_t handle) const {
    if ((handle & kDynamicHandleBit) == 0U) {
        return ResolveSystemHandle(handle);
    }
    const DynamicFontSlot* slot = FindDynamic(handle);
    return slot == nullptr ? nullptr : slot->loaded->font();
}

int32_t FontRegistry::LoadFont(std::span<const uint8_t> package, micropixel_font_info_t& info_out) {
    uint32_t index = 0U;
    while (index < dynamic_.size() && dynamic_[index].loaded != nullptr) {
        ++index;
    }
    if (index == dynamic_.size()) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    auto loaded = LoadFontCbin(package);
    if (!loaded) {
        return loaded.error() == FontCbinError::kOutOfMemory ? MICROPIXEL_STATUS_RESOURCE_EXHAUSTED
                                                             : MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    DynamicFontSlot& slot = dynamic_[index];
    slot.generation = static_cast<uint16_t>((slot.generation + 1U) & kDynamicGenerationMask);
    if (slot.generation == 0U) {
        slot.generation = 1U;
    }
    slot.loaded = std::move(*loaded);
    slot.scene_references = 0U;
    slot.guest_owned = true;
    const lv_font_t* font = slot.loaded->font();
    const int32_t ascent = font->line_height - font->base_line;
    info_out = {};
    info_out.size = sizeof(info_out);
    info_out.interface_major = MICROPIXEL_RESOURCE_INTERFACE_MAJOR;
    info_out.interface_minor = MICROPIXEL_RESOURCE_INTERFACE_MINOR;
    info_out.font = DynamicHandle(index, slot.generation);
    info_out.font_size = slot.loaded->size();
    info_out.line_height = static_cast<uint16_t>(font->line_height);
    info_out.ascent = static_cast<int16_t>(ascent);
    info_out.descent = static_cast<int16_t>(font->base_line);
    return MICROPIXEL_STATUS_OK;
}

void FontRegistry::Reclaim(DynamicFontSlot& slot) {
    if (!slot.guest_owned && slot.scene_references == 0U) {
        slot.loaded.reset();
    }
}

int32_t FontRegistry::ReleaseFont(micropixel_font_handle_t handle) {
    DynamicFontSlot* slot = FindDynamic(handle);
    if (slot == nullptr || !slot->guest_owned) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    slot->guest_owned = false;
    Reclaim(*slot);
    return MICROPIXEL_STATUS_OK;
}

bool FontRegistry::RetainSceneFont(micropixel_font_handle_t handle) {
    if ((handle & kDynamicHandleBit) == 0U) {
        return ResolveSystemHandle(handle) != nullptr;
    }
    DynamicFontSlot* slot = FindDynamic(handle);
    if (slot == nullptr || !slot->guest_owned || slot->scene_references == UINT16_MAX) {
        return false;
    }
    ++slot->scene_references;
    return true;
}

void FontRegistry::ReleaseSceneFont(micropixel_font_handle_t handle) {
    if ((handle & kDynamicHandleBit) == 0U) {
        return;
    }
    DynamicFontSlot* slot = FindDynamic(handle);
    if (slot == nullptr || slot->scene_references == 0U) {
        return;
    }
    --slot->scene_references;
    Reclaim(*slot);
}

void FontRegistry::ReleaseGuestFonts() {
    for (DynamicFontSlot& slot : dynamic_) {
        slot.guest_owned = false;
        Reclaim(slot);
    }
}

bool FontRegistry::Valid(const SystemFontSet& candidate) {
    for (const lv_font_t* font : candidate.fonts) {
        if (font == nullptr || font->get_glyph_dsc == nullptr || font->get_glyph_bitmap == nullptr ||
            font->line_height == 0U) {
            return false;
        }
    }
    return true;
}

bool FontRegistry::Activate(const SystemFontSet& candidate) {
    if (!Valid(candidate)) {
        return false;
    }
    active_ = candidate;
    ++generation_;
    return true;
}

void FontRegistry::ResetToBuiltin() {
    active_ = BuiltinFontSet();
    ++generation_;
}

}  // namespace micropixel::platform::lvgl
