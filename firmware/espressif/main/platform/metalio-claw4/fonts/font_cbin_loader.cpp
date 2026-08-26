#include "platform/metalio-claw4/fonts/font_cbin_loader.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

#include "device/font_cbin_format.h"
#include "psa/crypto.h"

namespace micropixel::platform::metalio_claw4 {

class FontCbinLoaderAccess final {
   public:
    static constexpr size_t MaxCmaps() { return LoadedCbinFont::kMaxCmaps; }
    static auto& Cmaps(LoadedCbinFont& loaded) { return loaded.cmaps_; }
    static auto& Descriptor(LoadedCbinFont& loaded) { return loaded.descriptor_; }
    static auto& KernPairs(LoadedCbinFont& loaded) { return loaded.kern_pairs_; }
    static auto& KernClasses(LoadedCbinFont& loaded) { return loaded.kern_classes_; }
    static auto& Font(LoadedCbinFont& loaded) { return loaded.font_; }
    static void SetPayload(LoadedCbinFont& loaded, std::span<const uint8_t> payload, uint32_t glyph_count,
                           uint16_t font_size) {
        loaded.payload_ = payload.data();
        loaded.payload_size_ = payload.size();
        loaded.glyph_count_ = glyph_count;
        loaded.size_ = font_size;
    }
};

namespace {

constexpr size_t kFontWireSize = 36U;
constexpr size_t kDescriptorWireSize = 24U;
constexpr size_t kGlyphDescriptorWireSize = 16U;
constexpr size_t kCmapWireSize = 20U;
constexpr size_t kKernPairWireSize = 12U;
constexpr size_t kKernClassesWireSize = 16U;
constexpr size_t kMaxPackageSize = 16U * 1024U * 1024U;
constexpr uint32_t kMaxGlyphs = 65536U;
constexpr uint32_t kMaxLineDimension = 4096U;
constexpr std::array<uint8_t, MICROPIXEL_FONT_CBIN_CONVERTER_COMMIT_SIZE> kConverterCommit{
    0xc4U, 0x20U, 0x99U, 0x9fU, 0xe7U, 0x9aU, 0xdbU, 0x0bU, 0xc2U, 0xa4U,
    0x80U, 0xc4U, 0xa6U, 0x4fU, 0xd3U, 0x3fU, 0xc6U, 0xe3U, 0x45U, 0x19U,
};

static_assert(std::endian::native == std::endian::little, "MicroPixel cbin v1 requires a little-endian target");
static_assert(LV_FONT_FMT_TXT_LARGE == 1, "MicroPixel cbin v1 requires LV_FONT_FMT_TXT_LARGE");
static_assert(sizeof(lv_font_fmt_txt_glyph_dsc_t) == kGlyphDescriptorWireSize, "LVGL glyph descriptor ABI changed");

uint16_t ReadU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t ReadU32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
           (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

int32_t ReadI32(const uint8_t* data) { return static_cast<int32_t>(ReadU32(data)); }

bool HasRange(size_t size, size_t offset, size_t length) { return offset <= size && length <= size - offset; }

bool IsAligned(uint32_t offset, uint32_t alignment) { return (offset & (alignment - 1U)) == 0U; }

bool AllZero(std::span<const uint8_t> bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value == 0U; });
}

bool ValidProfile(std::span<const uint8_t> profile) {
    const auto nul = std::find(profile.begin(), profile.end(), 0U);
    if (nul == profile.begin() || nul == profile.end()) {
        return false;
    }
    if (!std::all_of(profile.begin(), nul, [](uint8_t value) { return value >= 0x21U && value <= 0x7eU; })) {
        return false;
    }
    return std::all_of(nul, profile.end(), [](uint8_t value) { return value == 0U; });
}

bool ValidCodepointRange(uint32_t start, uint16_t length) {
    if (length == 0U || start > 0x10ffffU || static_cast<uint32_t>(length - 1U) > 0x10ffffU - start) {
        return false;
    }
    const uint32_t end = start + length - 1U;
    return end < 0xd800U || start > 0xdfffU;
}

bool ValidBpp(uint8_t bpp) { return bpp == 1U || bpp == 2U || bpp == 4U || bpp == 8U; }

bool ValidGlyphId(uint32_t glyph_id, uint32_t glyph_count) { return glyph_id < glyph_count; }

FontCbinError ValidateHeader(std::span<const uint8_t> package, std::span<const uint8_t>& payload_out,
                             uint16_t& font_size_out) {
    if (package.data() == nullptr) {
        return FontCbinError::kInvalidArgument;
    }
    if (package.size() < MICROPIXEL_FONT_CBIN_HEADER_SIZE) {
        return FontCbinError::kTruncated;
    }
    const uint8_t* header = package.data();
    if (std::memcmp(header, MICROPIXEL_FONT_CBIN_MAGIC, MICROPIXEL_FONT_CBIN_MAGIC_SIZE) != 0 ||
        ReadU16(header + MICROPIXEL_FONT_CBIN_OFFSET_HEADER_VERSION) != MICROPIXEL_FONT_CBIN_HEADER_VERSION ||
        ReadU16(header + MICROPIXEL_FONT_CBIN_OFFSET_HEADER_SIZE) != MICROPIXEL_FONT_CBIN_HEADER_SIZE ||
        ReadU32(header + MICROPIXEL_FONT_CBIN_OFFSET_FORMAT) != MICROPIXEL_FONT_CBIN_FORMAT_LVGL_V1) {
        return FontCbinError::kHeaderMismatch;
    }

    const uint32_t total_size = ReadU32(header + MICROPIXEL_FONT_CBIN_OFFSET_TOTAL_SIZE);
    const uint32_t payload_offset = ReadU32(header + MICROPIXEL_FONT_CBIN_OFFSET_PAYLOAD_OFFSET);
    const uint32_t payload_size = ReadU32(header + MICROPIXEL_FONT_CBIN_OFFSET_PAYLOAD_SIZE);
    if (total_size != package.size() || total_size > kMaxPackageSize ||
        payload_offset != MICROPIXEL_FONT_CBIN_HEADER_SIZE || payload_size != total_size - payload_offset ||
        payload_size < kFontWireSize + kDescriptorWireSize) {
        return total_size > kMaxPackageSize ? FontCbinError::kLimitExceeded : FontCbinError::kHeaderMismatch;
    }

    if (ReadU16(header + MICROPIXEL_FONT_CBIN_OFFSET_LVGL_MAJOR) != LVGL_VERSION_MAJOR ||
        ReadU16(header + MICROPIXEL_FONT_CBIN_OFFSET_LVGL_MINOR) != LVGL_VERSION_MINOR ||
        ReadU16(header + MICROPIXEL_FONT_CBIN_OFFSET_LVGL_PATCH) != LVGL_VERSION_PATCH ||
        header[MICROPIXEL_FONT_CBIN_OFFSET_ENDIAN] != MICROPIXEL_FONT_CBIN_ENDIAN_LITTLE ||
        header[MICROPIXEL_FONT_CBIN_OFFSET_POINTER_SIZE] != MICROPIXEL_FONT_CBIN_POINTER_SIZE ||
        header[MICROPIXEL_FONT_CBIN_OFFSET_GLYPH_DSC_LAYOUT] != MICROPIXEL_FONT_CBIN_GLYPH_DSC_LARGE) {
        return FontCbinError::kAbiMismatch;
    }
    const uint16_t font_size = ReadU16(header + MICROPIXEL_FONT_CBIN_OFFSET_FONT_SIZE_PX);
    if (header[MICROPIXEL_FONT_CBIN_OFFSET_FLAGS] != 0U || font_size == 0U || font_size > kMaxLineDimension ||
        !AllZero(package.subspan(MICROPIXEL_FONT_CBIN_OFFSET_RESERVED1, 4U)) ||
        std::memcmp(header + MICROPIXEL_FONT_CBIN_OFFSET_CONVERTER_COMMIT, kConverterCommit.data(),
                    kConverterCommit.size()) != 0 ||
        !ValidProfile(package.subspan(MICROPIXEL_FONT_CBIN_OFFSET_PROFILE, MICROPIXEL_FONT_CBIN_PROFILE_SIZE))) {
        return FontCbinError::kHeaderMismatch;
    }

    payload_out = package.subspan(payload_offset, payload_size);
    if ((reinterpret_cast<uintptr_t>(payload_out.data()) & 3U) != 0U) {
        return FontCbinError::kAbiMismatch;
    }
    std::array<uint8_t, MICROPIXEL_FONT_CBIN_SHA256_SIZE> digest{};
    size_t digest_size = 0U;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, payload_out.data(), payload_out.size(), digest.data(), digest.size(),
                         &digest_size) != PSA_SUCCESS ||
        digest_size != digest.size()) {
        return FontCbinError::kHashUnavailable;
    }
    if (std::memcmp(digest.data(), header + MICROPIXEL_FONT_CBIN_OFFSET_PAYLOAD_SHA256, digest.size()) != 0) {
        return FontCbinError::kHashMismatch;
    }
    font_size_out = font_size;
    return FontCbinError::kNone;
}

FontCbinError ValidateGlyphs(std::span<const uint8_t> payload, uint32_t bitmap_offset, uint32_t glyph_offset,
                             uint32_t cmap_offset, uint8_t bpp, uint32_t& glyph_count_out) {
    if (!IsAligned(bitmap_offset, 4U) || !IsAligned(glyph_offset, 4U) || !IsAligned(cmap_offset, 4U) ||
        bitmap_offset < kDescriptorWireSize || glyph_offset < bitmap_offset || cmap_offset <= glyph_offset ||
        !HasRange(payload.size(), kFontWireSize + bitmap_offset, glyph_offset - bitmap_offset) ||
        !HasRange(payload.size(), kFontWireSize + glyph_offset, cmap_offset - glyph_offset)) {
        return FontCbinError::kInvalidGlyphs;
    }
    const uint32_t glyph_bytes = cmap_offset - glyph_offset;
    if ((glyph_bytes % kGlyphDescriptorWireSize) != 0U) {
        return FontCbinError::kInvalidGlyphs;
    }
    const uint32_t wire_glyph_count = glyph_bytes / kGlyphDescriptorWireSize;
    if (wire_glyph_count < 3U || wire_glyph_count > kMaxGlyphs + 1U) {
        return FontCbinError::kLimitExceeded;
    }
    const uint32_t bitmap_size = glyph_offset - bitmap_offset;
    const uint8_t* glyphs = payload.data() + kFontWireSize + glyph_offset;
    if (!AllZero(std::span<const uint8_t>(glyphs + (wire_glyph_count - 1U) * kGlyphDescriptorWireSize,
                                          kGlyphDescriptorWireSize))) {
        return FontCbinError::kInvalidGlyphs;
    }
    const uint32_t glyph_count = wire_glyph_count - 1U;
    uint32_t previous_bitmap_index = 0U;
    for (uint32_t index = 0U; index < glyph_count; ++index) {
        const uint8_t* glyph = glyphs + index * kGlyphDescriptorWireSize;
        const uint32_t bitmap_index = ReadU32(glyph);
        const uint16_t box_width = ReadU16(glyph + 8U);
        const uint16_t box_height = ReadU16(glyph + 10U);
        if (box_width > kMaxLineDimension || box_height > kMaxLineDimension || bitmap_index < previous_bitmap_index ||
            bitmap_index > bitmap_size) {
            return FontCbinError::kInvalidGlyphs;
        }
        const uint64_t bitmap_bits = static_cast<uint64_t>(box_width) * box_height * bpp;
        const uint64_t bitmap_bytes = (bitmap_bits + 7U) / 8U;
        if (bitmap_bytes > bitmap_size - bitmap_index) {
            return FontCbinError::kInvalidGlyphs;
        }
        previous_bitmap_index = bitmap_index;
    }
    glyph_count_out = glyph_count;
    return FontCbinError::kNone;
}

FontCbinError ParseCmaps(std::span<const uint8_t> payload, uint32_t cmap_offset, uint16_t cmap_count,
                         uint32_t glyph_count, LoadedCbinFont& loaded) {
    if (cmap_count == 0U || cmap_count > FontCbinLoaderAccess::MaxCmaps()) {
        return cmap_count > FontCbinLoaderAccess::MaxCmaps() ? FontCbinError::kLimitExceeded
                                                             : FontCbinError::kInvalidCmap;
    }
    const size_t cmap_base_offset = kFontWireSize + cmap_offset;
    const size_t table_size = static_cast<size_t>(cmap_count) * kCmapWireSize;
    if (!HasRange(payload.size(), cmap_base_offset, table_size)) {
        return FontCbinError::kInvalidCmap;
    }
    const uint8_t* cmap_base = payload.data() + cmap_base_offset;
    uint32_t previous_range_end = 0U;
    bool has_previous = false;
    for (uint16_t index = 0U; index < cmap_count; ++index) {
        const uint8_t* wire = cmap_base + static_cast<size_t>(index) * kCmapWireSize;
        const uint32_t range_start = ReadU32(wire);
        const uint16_t range_length = ReadU16(wire + 4U);
        const uint16_t glyph_id_start = ReadU16(wire + 6U);
        const uint32_t unicode_offset = ReadU32(wire + 8U);
        const uint32_t glyph_offset = ReadU32(wire + 12U);
        const uint16_t list_length = ReadU16(wire + 16U);
        const uint8_t type = wire[18U];
        if (!ValidCodepointRange(range_start, range_length) || (has_previous && range_start <= previous_range_end) ||
            wire[19U] != 0U || type > LV_FONT_FMT_TXT_CMAP_SPARSE_TINY || !ValidGlyphId(glyph_id_start, glyph_count)) {
            return FontCbinError::kInvalidCmap;
        }
        previous_range_end = range_start + range_length - 1U;
        has_previous = true;

        size_t unicode_bytes = 0U;
        size_t glyph_bytes = 0U;
        switch (type) {
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL:
                if (unicode_offset != 0U || glyph_offset == 0U || list_length != range_length) {
                    return FontCbinError::kInvalidCmap;
                }
                glyph_bytes = list_length;
                break;
            case LV_FONT_FMT_TXT_CMAP_SPARSE_FULL:
                if (unicode_offset == 0U || glyph_offset == 0U || list_length == 0U) {
                    return FontCbinError::kInvalidCmap;
                }
                unicode_bytes = static_cast<size_t>(list_length) * sizeof(uint16_t);
                glyph_bytes = unicode_bytes;
                break;
            case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
                if (unicode_offset != 0U || glyph_offset != 0U || list_length != 0U ||
                    !ValidGlyphId(static_cast<uint32_t>(glyph_id_start) + range_length - 1U, glyph_count)) {
                    return FontCbinError::kInvalidCmap;
                }
                break;
            case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY:
                if (unicode_offset == 0U || glyph_offset != 0U || list_length == 0U ||
                    !ValidGlyphId(static_cast<uint32_t>(glyph_id_start) + list_length - 1U, glyph_count)) {
                    return FontCbinError::kInvalidCmap;
                }
                unicode_bytes = static_cast<size_t>(list_length) * sizeof(uint16_t);
                break;
            default:
                return FontCbinError::kInvalidCmap;
        }
        if ((unicode_offset != 0U && (!IsAligned(unicode_offset, 4U) || unicode_offset < table_size ||
                                      !HasRange(payload.size() - cmap_base_offset, unicode_offset, unicode_bytes))) ||
            (glyph_offset != 0U && (!IsAligned(glyph_offset, 4U) || glyph_offset < table_size ||
                                    !HasRange(payload.size() - cmap_base_offset, glyph_offset, glyph_bytes)))) {
            return FontCbinError::kInvalidCmap;
        }

        uint16_t previous_relative = 0U;
        for (uint16_t list_index = 0U; list_index < list_length && unicode_offset != 0U; ++list_index) {
            const uint16_t relative = ReadU16(cmap_base + unicode_offset + static_cast<size_t>(list_index) * 2U);
            if (relative >= range_length || (list_index != 0U && relative <= previous_relative)) {
                return FontCbinError::kInvalidCmap;
            }
            previous_relative = relative;
        }
        for (uint16_t list_index = 0U; list_index < list_length && glyph_offset != 0U; ++list_index) {
            const uint32_t glyph_delta = type == LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL
                                             ? cmap_base[glyph_offset + list_index]
                                             : ReadU16(cmap_base + glyph_offset + static_cast<size_t>(list_index) * 2U);
            if (!ValidGlyphId(static_cast<uint32_t>(glyph_id_start) + glyph_delta, glyph_count)) {
                return FontCbinError::kInvalidCmap;
            }
        }

        FontCbinLoaderAccess::Cmaps(loaded)[index] = lv_font_fmt_txt_cmap_t{
            .range_start = range_start,
            .range_length = range_length,
            .glyph_id_start = glyph_id_start,
            .unicode_list =
                unicode_offset == 0U ? nullptr : reinterpret_cast<const uint16_t*>(cmap_base + unicode_offset),
            .glyph_id_ofs_list = glyph_offset == 0U ? nullptr : cmap_base + glyph_offset,
            .list_length = list_length,
            .type = static_cast<lv_font_fmt_txt_cmap_type_t>(type),
        };
    }
    return FontCbinError::kNone;
}

FontCbinError ParseKerning(std::span<const uint8_t> payload, uint32_t kern_offset, uint32_t bitmap_offset,
                           bool kern_classes, uint32_t glyph_count, LoadedCbinFont& loaded) {
    if (kern_offset == 0U) {
        return (!kern_classes && bitmap_offset == kDescriptorWireSize) ? FontCbinError::kNone
                                                                       : FontCbinError::kInvalidKerning;
    }
    if (kern_offset != kDescriptorWireSize || bitmap_offset <= kern_offset || !IsAligned(bitmap_offset, 4U)) {
        return FontCbinError::kInvalidKerning;
    }
    const size_t kern_base_offset = kFontWireSize + kern_offset;
    const size_t kern_size = bitmap_offset - kern_offset;
    const uint8_t* kern = payload.data() + kern_base_offset;
    if (kern_classes) {
        if (kern_size < kKernClassesWireSize) {
            return FontCbinError::kInvalidKerning;
        }
        const uint32_t values_offset = ReadU32(kern);
        const uint32_t left_offset = ReadU32(kern + 4U);
        const uint32_t right_offset = ReadU32(kern + 8U);
        const uint8_t left_count = kern[12U];
        const uint8_t right_count = kern[13U];
        const size_t values_size = static_cast<size_t>(left_count) * right_count;
        if (left_count == 0U || right_count == 0U || kern[14U] != 0U || kern[15U] != 0U ||
            values_offset < kKernClassesWireSize || !IsAligned(values_offset, 4U) || !IsAligned(left_offset, 4U) ||
            !IsAligned(right_offset, 4U) || !HasRange(kern_size, values_offset, values_size) ||
            left_offset < values_offset + values_size || !HasRange(kern_size, left_offset, glyph_count) ||
            right_offset < left_offset + glyph_count || !HasRange(kern_size, right_offset, glyph_count)) {
            return FontCbinError::kInvalidKerning;
        }
        for (uint32_t index = 0U; index < glyph_count; ++index) {
            if (kern[left_offset + index] > left_count || kern[right_offset + index] > right_count) {
                return FontCbinError::kInvalidKerning;
            }
        }
        FontCbinLoaderAccess::KernClasses(loaded) = lv_font_fmt_txt_kern_classes_t{
            .class_pair_values = reinterpret_cast<const int8_t*>(kern + values_offset),
            .left_class_mapping = kern + left_offset,
            .right_class_mapping = kern + right_offset,
            .left_class_cnt = left_count,
            .right_class_cnt = right_count,
        };
        FontCbinLoaderAccess::Descriptor(loaded).kern_dsc = &FontCbinLoaderAccess::KernClasses(loaded);
        return FontCbinError::kNone;
    }

    if (kern_size < kKernPairWireSize) {
        return FontCbinError::kInvalidKerning;
    }
    const uint32_t glyph_ids_offset = ReadU32(kern);
    const uint32_t values_offset = ReadU32(kern + 4U);
    const uint32_t packed = ReadU32(kern + 8U);
    const uint32_t pair_count = packed & 0x3fffffffU;
    const uint8_t glyph_ids_size = static_cast<uint8_t>(packed >> 30U);
    const size_t glyph_id_bytes = glyph_ids_size == 0U ? 1U : 2U;
    if (pair_count == 0U || glyph_ids_size > 1U || glyph_ids_offset < kKernPairWireSize ||
        !IsAligned(glyph_ids_offset, 4U) || !IsAligned(values_offset, 4U) ||
        pair_count > (std::numeric_limits<size_t>::max() / (2U * glyph_id_bytes)) ||
        !HasRange(kern_size, glyph_ids_offset, static_cast<size_t>(pair_count) * 2U * glyph_id_bytes) ||
        values_offset < glyph_ids_offset + static_cast<size_t>(pair_count) * 2U * glyph_id_bytes ||
        !HasRange(kern_size, values_offset, pair_count)) {
        return FontCbinError::kInvalidKerning;
    }
    for (uint32_t index = 0U; index < pair_count * 2U; ++index) {
        const uint32_t glyph_id =
            glyph_ids_size == 0U ? kern[glyph_ids_offset + index] : ReadU16(kern + glyph_ids_offset + index * 2U);
        if (!ValidGlyphId(glyph_id, glyph_count)) {
            return FontCbinError::kInvalidKerning;
        }
    }
    FontCbinLoaderAccess::KernPairs(loaded).glyph_ids = kern + glyph_ids_offset;
    FontCbinLoaderAccess::KernPairs(loaded).values = reinterpret_cast<const int8_t*>(kern + values_offset);
    FontCbinLoaderAccess::KernPairs(loaded).pair_cnt = pair_count;
    FontCbinLoaderAccess::KernPairs(loaded).glyph_ids_size = glyph_ids_size;
    FontCbinLoaderAccess::Descriptor(loaded).kern_dsc = &FontCbinLoaderAccess::KernPairs(loaded);
    return FontCbinError::kNone;
}

FontCbinError ParsePayload(std::span<const uint8_t> payload, uint16_t font_size, LoadedCbinFont& loaded) {
    const uint8_t* font = payload.data();
    if (!AllZero(payload.first(12U)) || ReadU32(font + 24U) != kFontWireSize || ReadU32(font + 28U) != 0U ||
        ReadU32(font + 32U) != 0U || font[23U] != 0U) {
        return FontCbinError::kInvalidFont;
    }
    const int32_t line_height = ReadI32(font + 12U);
    const int32_t base_line = ReadI32(font + 16U);
    const uint8_t font_flags = font[20U];
    if (line_height <= 0 || line_height > static_cast<int32_t>(kMaxLineDimension) ||
        base_line < -static_cast<int32_t>(kMaxLineDimension) || base_line > static_cast<int32_t>(kMaxLineDimension) ||
        (font_flags & 0xf0U) != 0U) {
        return FontCbinError::kInvalidFont;
    }

    const uint8_t* descriptor = font + kFontWireSize;
    const uint32_t bitmap_offset = ReadU32(descriptor);
    const uint32_t glyph_offset = ReadU32(descriptor + 4U);
    const uint32_t cmap_offset = ReadU32(descriptor + 8U);
    const uint32_t kern_offset = ReadU32(descriptor + 12U);
    const uint16_t kern_scale = ReadU16(descriptor + 16U);
    const uint16_t descriptor_flags = ReadU16(descriptor + 18U);
    const uint16_t cmap_count = descriptor_flags & 0x01ffU;
    const uint8_t bpp = static_cast<uint8_t>((descriptor_flags >> 9U) & 0x0fU);
    const bool kern_classes = ((descriptor_flags >> 13U) & 1U) != 0U;
    const uint8_t bitmap_format = static_cast<uint8_t>(descriptor_flags >> 14U);
    if (!ValidBpp(bpp) || bitmap_format != LV_FONT_FMT_TXT_PLAIN || descriptor[20U] != 0U ||
        !AllZero(payload.subspan(kFontWireSize + 21U, 3U))) {
        return FontCbinError::kInvalidFont;
    }

    uint32_t glyph_count = 0U;
    FontCbinError error = ValidateGlyphs(payload, bitmap_offset, glyph_offset, cmap_offset, bpp, glyph_count);
    if (error != FontCbinError::kNone) {
        return error;
    }
    error = ParseCmaps(payload, cmap_offset, cmap_count, glyph_count, loaded);
    if (error != FontCbinError::kNone) {
        return error;
    }

    auto& loaded_descriptor = FontCbinLoaderAccess::Descriptor(loaded);
    loaded_descriptor.glyph_bitmap = payload.data() + kFontWireSize + bitmap_offset;
    loaded_descriptor.glyph_dsc =
        reinterpret_cast<const lv_font_fmt_txt_glyph_dsc_t*>(payload.data() + kFontWireSize + glyph_offset);
    loaded_descriptor.cmaps = FontCbinLoaderAccess::Cmaps(loaded).data();
    loaded_descriptor.kern_scale = kern_scale;
    loaded_descriptor.cmap_num = cmap_count;
    loaded_descriptor.bpp = bpp;
    loaded_descriptor.kern_classes = kern_classes ? 1U : 0U;
    loaded_descriptor.bitmap_format = bitmap_format;
    loaded_descriptor.stride = 0U;
    error = ParseKerning(payload, kern_offset, bitmap_offset, kern_classes, glyph_count, loaded);
    if (error != FontCbinError::kNone) {
        return error;
    }

    auto& loaded_font = FontCbinLoaderAccess::Font(loaded);
    loaded_font.get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt;
    loaded_font.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
    loaded_font.release_glyph = nullptr;
    loaded_font.line_height = line_height;
    loaded_font.base_line = base_line;
    loaded_font.subpx = font_flags & 0x03U;
    loaded_font.kerning = (font_flags >> 2U) & 1U;
    loaded_font.static_bitmap = (font_flags >> 3U) & 1U;
    loaded_font.underline_position = static_cast<int8_t>(font[21U]);
    loaded_font.underline_thickness = static_cast<int8_t>(font[22U]);
    loaded_font.dsc = &loaded_descriptor;
    loaded_font.fallback = nullptr;
    loaded_font.user_data = nullptr;
    FontCbinLoaderAccess::SetPayload(loaded, payload, glyph_count, font_size);
    return FontCbinError::kNone;
}

}  // namespace

std::expected<std::unique_ptr<LoadedCbinFont>, FontCbinError> LoadFontCbin(std::span<const uint8_t> package) {
    std::span<const uint8_t> payload;
    uint16_t font_size = 0U;
    const FontCbinError header_error = ValidateHeader(package, payload, font_size);
    if (header_error != FontCbinError::kNone) {
        return std::unexpected(header_error);
    }
    std::unique_ptr<LoadedCbinFont> loaded(new (std::nothrow) LoadedCbinFont());
    if (!loaded) {
        return std::unexpected(FontCbinError::kOutOfMemory);
    }
    const FontCbinError payload_error = ParsePayload(payload, font_size, *loaded);
    if (payload_error != FontCbinError::kNone) {
        return std::unexpected(payload_error);
    }
    return loaded;
}

}  // namespace micropixel::platform::metalio_claw4
