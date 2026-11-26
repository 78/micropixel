// SPDX-License-Identifier: Apache-2.0
#include "runtime/resources/bitmap_decoder.hpp"

#include <climits>
#include <cstring>

#include "esp_heap_caps.h"
#include "runtime/bundle/bundle_section_reader.hpp"
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 4096
#define STBI_MALLOC(n) heap_caps_malloc(n, MALLOC_CAP_SPIRAM)
#define STBI_REALLOC(p, n) heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM)
#define STBI_FREE(p) heap_caps_free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
namespace micropixel::runtime {
namespace {
uint32_t Bpp(uint32_t f) {
    return f == MICROPIXEL_PIXEL_FORMAT_BGR888     ? 3
           : f == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? 4
           : f == MICROPIXEL_PIXEL_FORMAT_RGB565   ? 2
                                                   : 0;
}
constexpr uint64_t kMaxBytes = 6U * 1024 * 1024;
bool Decode(const uint8_t* data, uint32_t size, uint32_t ew, uint32_t eh, const PngDecodeOptions& o,
            DecodedBitmap& out) {
    if (!data || !size || size > INT_MAX || !o.scale_denominator || !o.scale_numerator || o.scale_denominator > 4096 ||
        o.scale_numerator > 4096 || out.valid())
        return false;
    int w{}, h{}, c{};
    if (!stbi_info_from_memory(data, size, &w, &h, &c) || w <= 0 || h <= 0 || (ew && ew != (uint32_t)w) ||
        (eh && eh != (uint32_t)h) || (uint64_t)w * h * 4 > kMaxBytes)
        return false;
    uint32_t format = c == 2 || c == 4 ? MICROPIXEL_PIXEL_FORMAT_BGRA8888 : o.preferred_opaque_format;
    if (!Bpp(format)) return false;
    uint64_t width = ((uint64_t)w * o.scale_numerator + o.scale_denominator / 2) / o.scale_denominator,
             height = ((uint64_t)h * o.scale_numerator + o.scale_denominator / 2) / o.scale_denominator;
    if (!width || !height || width > 4096 || height > 4096) return false;
    stbi_uc* rgba = stbi_load_from_memory(data, size, &w, &h, &c, 4);
    if (!rgba) return false;
    if (!AllocateBitmap(width, height, format, out, o.stride_alignment_pixels)) {
        stbi_image_free(rgba);
        return false;
    }
    auto& v = out.view();
    auto* dest = const_cast<uint8_t*>(v.data);
    memset(dest, 0, v.size);
    for (uint32_t y = 0; y < v.height; ++y) {
        uint32_t sy = v.height > 1 ? (uint64_t)y * (h - 1) / (v.height - 1) : 0;
        for (uint32_t x = 0; x < v.width; ++x) {
            uint32_t sx = v.width > 1 ? (uint64_t)x * (w - 1) / (v.width - 1) : 0;
            auto* p = rgba + ((size_t)sy * w + sx) * 4;
            auto* d = dest + (size_t)y * v.stride + x * Bpp(format);
            if (format == MICROPIXEL_PIXEL_FORMAT_RGB565) {
                uint16_t rgb = (p[0] >> 3) << 11 | (p[1] >> 2) << 5 | (p[2] >> 3);
                d[0] = rgb;
                d[1] = rgb >> 8;
            } else {
                d[0] = p[2];
                d[1] = p[1];
                d[2] = p[0];
                if (Bpp(format) == 4) d[3] = p[3];
            }
        }
    }
    stbi_image_free(rgba);
    return true;
}
}  // namespace
DecodedBitmap::~DecodedBitmap() { heap_caps_free(const_cast<uint8_t*>(view_.data)); }
void DecodedBitmap::ReleaseOwnership() { view_.data = nullptr; }
bool AllocateBitmap(uint32_t w, uint32_t h, uint32_t f, DecodedBitmap& out, uint32_t align) {
    uint32_t bpp = Bpp(f);
    if (out.valid() || !w || !h || w > 4096 || h > 4096 || !bpp || !align || align > 4096 || (align & (align - 1)))
        return false;
    uint64_t stride = (((uint64_t)w + align - 1) / align) * align * bpp, size = stride * h;
    if (size > kMaxBytes || size > UINT32_MAX) return false;
    auto* p = (uint8_t*)heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM);
    if (!p) return false;
    out.view_ = {p, (uint32_t)size, w, h, (uint32_t)stride, f};
    return true;
}
bool DecodeBitmap(const micropixel_bundle_asset_view_t& a, DecodedBitmap& out) {
    return DecodeBitmap(a, MICROPIXEL_PIXEL_FORMAT_BGR888, out);
}
bool DecodeBitmap(const micropixel_bundle_asset_view_t& a, uint32_t f, DecodedBitmap& out) {
    if (a.format != MICROPIXEL_BUNDLE_FORMAT_PNG && a.format != MICROPIXEL_BUNDLE_FORMAT_JPEG) return false;
    if (a.format == MICROPIXEL_BUNDLE_FORMAT_PNG) return DecodePngBitmap(a, PngDecodeOptions{f}, out);
    strcpy(out.failure_detail_.data(), "Invalid or oversized JPEG bitmap");
    return Decode(a.data, a.size, a.width, a.height, PngDecodeOptions{f}, out);
}
}  // namespace micropixel::runtime
