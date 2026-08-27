#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "device/font_cbin_format.h"
#include "platform/lvgl/fonts/font_cbin_loader.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "psa/crypto.h"

namespace {

constexpr size_t kPayloadOffset = MICROPIXEL_FONT_CBIN_HEADER_SIZE;
constexpr size_t kFontSize = 36U;
constexpr size_t kDescriptorSize = 24U;
constexpr size_t kBitmapOffset = kDescriptorSize;
constexpr size_t kGlyphOffset = kBitmapOffset + 4U;
constexpr size_t kCmapOffset = kGlyphOffset + 48U;
constexpr size_t kPayloadSize = kFontSize + kCmapOffset + 20U;
constexpr std::array<uint8_t, 20U> kConverterCommit{
    0xc4U, 0x20U, 0x99U, 0x9fU, 0xe7U, 0x9aU, 0xdbU, 0x0bU, 0xc2U, 0xa4U,
    0x80U, 0xc4U, 0xa6U, 0x4fU, 0xd3U, 0x3fU, 0xc6U, 0xe3U, 0x45U, 0x19U,
};

void PutU16(std::vector<uint8_t>& data, size_t offset, uint16_t value) {
    data[offset] = static_cast<uint8_t>(value);
    data[offset + 1U] = static_cast<uint8_t>(value >> 8U);
}

void PutU32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    data[offset] = static_cast<uint8_t>(value);
    data[offset + 1U] = static_cast<uint8_t>(value >> 8U);
    data[offset + 2U] = static_cast<uint8_t>(value >> 16U);
    data[offset + 3U] = static_cast<uint8_t>(value >> 24U);
}

void Seal(std::vector<uint8_t>& package) {
    std::array<uint8_t, 32U> digest{};
    size_t digest_size = 0U;
    assert(psa_hash_compute(PSA_ALG_SHA_256, package.data() + kPayloadOffset, package.size() - kPayloadOffset,
                            digest.data(), digest.size(), &digest_size) == PSA_SUCCESS);
    assert(digest_size == digest.size());
    std::memcpy(package.data() + MICROPIXEL_FONT_CBIN_OFFSET_PAYLOAD_SHA256, digest.data(), digest.size());
}

std::vector<uint8_t> ValidPackage() {
    std::vector<uint8_t> package(kPayloadOffset + kPayloadSize, 0U);
    std::memcpy(package.data(), MICROPIXEL_FONT_CBIN_MAGIC, MICROPIXEL_FONT_CBIN_MAGIC_SIZE);
    PutU16(package, MICROPIXEL_FONT_CBIN_OFFSET_HEADER_VERSION, MICROPIXEL_FONT_CBIN_HEADER_VERSION);
    PutU16(package, MICROPIXEL_FONT_CBIN_OFFSET_HEADER_SIZE, MICROPIXEL_FONT_CBIN_HEADER_SIZE);
    PutU32(package, MICROPIXEL_FONT_CBIN_OFFSET_TOTAL_SIZE, package.size());
    PutU32(package, MICROPIXEL_FONT_CBIN_OFFSET_PAYLOAD_OFFSET, kPayloadOffset);
    PutU32(package, MICROPIXEL_FONT_CBIN_OFFSET_PAYLOAD_SIZE, kPayloadSize);
    PutU32(package, MICROPIXEL_FONT_CBIN_OFFSET_FORMAT, MICROPIXEL_FONT_CBIN_FORMAT_LVGL_V1);
    PutU16(package, MICROPIXEL_FONT_CBIN_OFFSET_LVGL_MAJOR, LVGL_VERSION_MAJOR);
    PutU16(package, MICROPIXEL_FONT_CBIN_OFFSET_LVGL_MINOR, LVGL_VERSION_MINOR);
    PutU16(package, MICROPIXEL_FONT_CBIN_OFFSET_LVGL_PATCH, LVGL_VERSION_PATCH);
    package[MICROPIXEL_FONT_CBIN_OFFSET_ENDIAN] = MICROPIXEL_FONT_CBIN_ENDIAN_LITTLE;
    package[MICROPIXEL_FONT_CBIN_OFFSET_POINTER_SIZE] = MICROPIXEL_FONT_CBIN_POINTER_SIZE;
    package[MICROPIXEL_FONT_CBIN_OFFSET_GLYPH_DSC_LAYOUT] = MICROPIXEL_FONT_CBIN_GLYPH_DSC_LARGE;
    PutU16(package, MICROPIXEL_FONT_CBIN_OFFSET_FONT_SIZE_PX, 16U);
    std::memcpy(package.data() + MICROPIXEL_FONT_CBIN_OFFSET_CONVERTER_COMMIT, kConverterCommit.data(),
                kConverterCommit.size());
    std::memcpy(package.data() + MICROPIXEL_FONT_CBIN_OFFSET_PROFILE, "fixture-v1", sizeof("fixture-v1"));

    const size_t font = kPayloadOffset;
    PutU32(package, font + 12U, 16U);
    PutU32(package, font + 16U, 3U);
    package[font + 22U] = 1U;
    PutU32(package, font + 24U, kFontSize);

    const size_t descriptor = font + kFontSize;
    PutU32(package, descriptor, kBitmapOffset);
    PutU32(package, descriptor + 4U, kGlyphOffset);
    PutU32(package, descriptor + 8U, kCmapOffset);
    PutU16(package, descriptor + 16U, 16U);
    PutU16(package, descriptor + 18U, 1U | (4U << 9U));
    package[descriptor + kBitmapOffset] = 0xf0U;

    const size_t glyph = descriptor + kGlyphOffset + 16U;
    PutU32(package, glyph + 4U, 16U);
    PutU16(package, glyph + 8U, 1U);
    PutU16(package, glyph + 10U, 1U);

    const size_t cmap = descriptor + kCmapOffset;
    PutU32(package, cmap, 0x41U);
    PutU16(package, cmap + 4U, 1U);
    PutU16(package, cmap + 6U, 1U);
    package[cmap + 18U] = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY;
    Seal(package);
    return package;
}

}  // namespace

bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t*, lv_font_glyph_dsc_t*, uint32_t, uint32_t) { return true; }

const void* lv_font_get_bitmap_fmt_txt(lv_font_glyph_dsc_t*, lv_draw_buf_t*) { return nullptr; }

extern "C" {
extern const lv_font_t font_builtin_latin_14{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 18,
};
extern const lv_font_t font_builtin_latin_18{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 23,
};
extern const lv_font_t font_builtin_latin_24{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 30,
};
extern const lv_font_t font_builtin_latin_32{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 39,
};
}

int main() {
    using micropixel::platform::lvgl::FontCbinError;
    using micropixel::platform::lvgl::LoadFontCbin;

    auto package = ValidPackage();
    auto loaded = LoadFontCbin(package);
    assert(loaded.has_value());
    assert((*loaded)->font()->line_height == 16);
    assert((*loaded)->glyph_count() == 2U);
    assert((*loaded)->size() == 16U);
    assert((*loaded)->payload().size() == kPayloadSize);

    micropixel::platform::lvgl::FontRegistry registry;
    micropixel_font_info_t info{};
    assert(registry.LoadFont(package, info) == MICROPIXEL_STATUS_OK);
    assert(info.font != 0U && info.font_size == 16U && info.line_height == 16U);
    const micropixel_font_handle_t first_handle = info.font;
    assert(registry.ResolveGuestHandle(first_handle) != nullptr);
    assert(registry.RetainSceneFont(first_handle));
    assert(registry.ReleaseFont(first_handle) == MICROPIXEL_STATUS_OK);
    assert(registry.ResolveGuestHandle(first_handle) == nullptr);
    assert(registry.ResolveRetainedHandle(first_handle) != nullptr);
    registry.ReleaseSceneFont(first_handle);
    assert(registry.ResolveRetainedHandle(first_handle) == nullptr);
    assert(registry.ReleaseFont(first_handle) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    assert(registry.LoadFont(package, info) == MICROPIXEL_STATUS_OK);
    assert(info.font != first_handle);
    registry.ReleaseGuestFonts();
    assert(registry.ResolveRetainedHandle(info.font) == nullptr);

    auto truncated = package;
    truncated.resize(MICROPIXEL_FONT_CBIN_HEADER_SIZE - 1U);
    auto result = LoadFontCbin(truncated);
    assert(!result && result.error() == FontCbinError::kTruncated);

    auto wrong_abi = package;
    wrong_abi[MICROPIXEL_FONT_CBIN_OFFSET_GLYPH_DSC_LAYOUT] = 0U;
    result = LoadFontCbin(wrong_abi);
    assert(!result && result.error() == FontCbinError::kAbiMismatch);

    auto invalid_size = package;
    PutU16(invalid_size, MICROPIXEL_FONT_CBIN_OFFSET_FONT_SIZE_PX, 0U);
    result = LoadFontCbin(invalid_size);
    assert(!result && result.error() == FontCbinError::kHeaderMismatch);

    auto wrong_hash = package;
    wrong_hash.back() ^= 0x01U;
    result = LoadFontCbin(wrong_hash);
    assert(!result && result.error() == FontCbinError::kHashMismatch);

    auto invalid_glyph = package;
    PutU32(invalid_glyph, kPayloadOffset + kFontSize + kGlyphOffset + 16U, 5U);
    Seal(invalid_glyph);
    result = LoadFontCbin(invalid_glyph);
    assert(!result && result.error() == FontCbinError::kInvalidGlyphs);

    auto invalid_cmap = package;
    PutU16(invalid_cmap, kPayloadOffset + kFontSize + kCmapOffset + 4U, 0U);
    Seal(invalid_cmap);
    result = LoadFontCbin(invalid_cmap);
    assert(!result && result.error() == FontCbinError::kInvalidCmap);

    auto invalid_offset = package;
    PutU32(invalid_offset, kPayloadOffset + kFontSize + 8U, 0xfffffff0U);
    Seal(invalid_offset);
    result = LoadFontCbin(invalid_offset);
    assert(!result && result.error() == FontCbinError::kInvalidGlyphs);

    return 0;
}
