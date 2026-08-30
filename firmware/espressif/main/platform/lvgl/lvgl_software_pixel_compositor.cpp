#include "platform/lvgl/lvgl_software_pixel_compositor.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "lvgl.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_to_rgb565.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_to_rgb888.h"
#include "src/draw/sw/lv_draw_sw.h"

namespace micropixel::platform::lvgl {
namespace {

using graphics::ConstPixelSurface;
using graphics::PixelSurface;
using graphics::SurfacePixelFormat;
using graphics::SurfaceRect;

uint32_t BytesPerPixel(SurfacePixelFormat format) {
    switch (format) {
        case SurfacePixelFormat::kBgr888:
            return 3U;
        case SurfacePixelFormat::kBgra8888:
            return 4U;
        case SurfacePixelFormat::kRgb565:
            return 2U;
    }
    return 0U;
}

lv_color_format_t LvColorFormat(SurfacePixelFormat format) {
    switch (format) {
        case SurfacePixelFormat::kBgr888:
            return LV_COLOR_FORMAT_RGB888;
        case SurfacePixelFormat::kBgra8888:
            return LV_COLOR_FORMAT_ARGB8888;
        case SurfacePixelFormat::kRgb565:
            return LV_COLOR_FORMAT_RGB565;
    }
    return LV_COLOR_FORMAT_UNKNOWN;
}

template <typename Surface>
bool ValidSurface(const Surface& surface) {
    const uint32_t bytes_per_pixel = BytesPerPixel(surface.format);
    if (surface.pixels == nullptr || surface.width == 0U || surface.height == 0U || bytes_per_pixel == 0U ||
        surface.width > INT32_MAX || surface.height > INT32_MAX || surface.origin_x > INT32_MAX ||
        surface.origin_y > INT32_MAX || surface.width > UINT32_MAX - surface.origin_x ||
        surface.origin_x + surface.width > UINT32_MAX / bytes_per_pixel) {
        return false;
    }
    const uint32_t row_bytes = (surface.origin_x + surface.width) * bytes_per_pixel;
    const uint64_t required_bytes =
        static_cast<uint64_t>(surface.stride) * (surface.origin_y + surface.height - 1U) + row_bytes;
    return surface.stride >= row_bytes && required_bytes <= surface.size;
}

bool ValidDestinationSurface(const PixelSurface& surface) {
    return ValidSurface(surface) && surface.format != SurfacePixelFormat::kBgra8888;
}

bool ValidSourceRect(const ConstPixelSurface& source, const SurfaceRect& rect) {
    return rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0 &&
           static_cast<int64_t>(rect.x) + rect.width <= source.width &&
           static_cast<int64_t>(rect.y) + rect.height <= source.height;
}

bool ValidDestinationRect(const SurfaceRect& rect) {
    if (rect.width <= 0 || rect.height <= 0) {
        return false;
    }
    const int64_t right = static_cast<int64_t>(rect.x) + rect.width;
    const int64_t bottom = static_cast<int64_t>(rect.y) + rect.height;
    return right >= INT32_MIN && right <= INT32_MAX && bottom >= INT32_MIN && bottom <= INT32_MAX;
}

SurfaceRect ClipToSurface(const SurfaceRect& rect, const PixelSurface& surface) {
    const int64_t left = rect.x > 0 ? rect.x : 0;
    const int64_t top = rect.y > 0 ? rect.y : 0;
    const int64_t rect_right = static_cast<int64_t>(rect.x) + rect.width;
    const int64_t rect_bottom = static_cast<int64_t>(rect.y) + rect.height;
    const int64_t right = rect_right < surface.width ? rect_right : surface.width;
    const int64_t bottom = rect_bottom < surface.height ? rect_bottom : surface.height;
    if (right <= left || bottom <= top) {
        return {};
    }
    return {
        .x = static_cast<int32_t>(left),
        .y = static_cast<int32_t>(top),
        .width = static_cast<int32_t>(right - left),
        .height = static_cast<int32_t>(bottom - top),
    };
}

uint8_t* MutablePixelAt(PixelSurface surface, uint32_t x, uint32_t y) {
    return surface.pixels + static_cast<size_t>(surface.origin_y + y) * surface.stride +
           (surface.origin_x + x) * BytesPerPixel(surface.format);
}

const uint8_t* PixelAt(ConstPixelSurface surface, uint32_t x, uint32_t y) {
    return surface.pixels + static_cast<size_t>(surface.origin_y + y) * surface.stride +
           (surface.origin_x + x) * BytesPerPixel(surface.format);
}

lv_area_t LocalArea(int32_t width, int32_t height) { return {0, 0, width - 1, height - 1}; }

bool ScaleFactor(int32_t source, int32_t destination, int32_t& scale) {
    const uint64_t scaled = (static_cast<uint64_t>(destination) * LV_SCALE_NONE + static_cast<uint32_t>(source) / 2U) /
                            static_cast<uint32_t>(source);
    if (scaled == 0U || scaled > INT32_MAX) {
        return false;
    }
    scale = static_cast<int32_t>(scaled);
    return true;
}

constexpr uint32_t MapScaledEndpoint(uint32_t destination_index, uint32_t destination_extent, uint32_t source_extent) {
    if (destination_extent <= 1U || source_extent <= 1U) {
        return 0U;
    }
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(destination_index) * (source_extent - 1U) + (destination_extent - 1U) / 2U) /
        (destination_extent - 1U));
}

static_assert(MapScaledEndpoint(416U, 417U, 625U) == 624U);

void PreserveScaledFarEdges(const device::BitmapView& source, const device::BitmapView& destination,
                            uint32_t bytes_per_pixel) {
    auto* output = const_cast<uint8_t*>(destination.data);
    const uint32_t destination_right = destination.width - 1U;
    const uint32_t source_right = source.width - 1U;
    for (uint32_t y = 0U; y < destination.height; ++y) {
        const uint32_t source_y = MapScaledEndpoint(y, destination.height, source.height);
        const uint8_t* source_pixel = source.data + static_cast<size_t>(source_y) * source.stride +
                                      static_cast<size_t>(source_right) * bytes_per_pixel;
        uint8_t* destination_pixel = output + static_cast<size_t>(y) * destination.stride +
                                     static_cast<size_t>(destination_right) * bytes_per_pixel;
        std::memcpy(destination_pixel, source_pixel, bytes_per_pixel);
    }

    const uint32_t destination_bottom = destination.height - 1U;
    const uint32_t source_bottom = source.height - 1U;
    const uint8_t* source_row = source.data + static_cast<size_t>(source_bottom) * source.stride;
    uint8_t* destination_row = output + static_cast<size_t>(destination_bottom) * destination.stride;
    for (uint32_t x = 0U; x < destination.width; ++x) {
        const uint32_t source_x = MapScaledEndpoint(x, destination.width, source.width);
        std::memcpy(destination_row + static_cast<size_t>(x) * bytes_per_pixel,
                    source_row + static_cast<size_t>(source_x) * bytes_per_pixel, bytes_per_pixel);
    }
}

void BlendFill(lv_draw_sw_blend_fill_dsc_t& descriptor, SurfacePixelFormat destination_format) {
    if (destination_format == SurfacePixelFormat::kRgb565) {
        lv_draw_sw_blend_color_to_rgb565(&descriptor);
    } else {
        lv_draw_sw_blend_color_to_rgb888(&descriptor, 3U);
    }
}

void BlendImage(lv_draw_sw_blend_image_dsc_t& descriptor, SurfacePixelFormat destination_format) {
    if (destination_format == SurfacePixelFormat::kRgb565) {
        lv_draw_sw_blend_image_to_rgb565(&descriptor);
    } else {
        lv_draw_sw_blend_image_to_rgb888(&descriptor, 3U);
    }
}

}  // namespace

void LvglSoftwarePixelCompositor::SetTransformScratch(uint8_t* pixels, uint32_t size) {
    transform_scratch_ = pixels;
    transform_scratch_size_ = pixels == nullptr ? 0U : size;
}

bool LvglSoftwarePixelCompositor::ScaleBitmap(const device::BitmapView& source, const device::BitmapView& destination) {
    uint32_t bytes_per_pixel = 0U;
    lv_color_format_t source_format = LV_COLOR_FORMAT_UNKNOWN;
    if (source.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888) {
        bytes_per_pixel = 3U;
        source_format = LV_COLOR_FORMAT_RGB888;
    } else if (source.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888) {
        bytes_per_pixel = 4U;
        source_format = LV_COLOR_FORMAT_ARGB8888;
    } else {
        return false;
    }
    const uint64_t source_row_bytes = static_cast<uint64_t>(source.width) * bytes_per_pixel;
    const uint64_t destination_row_bytes = static_cast<uint64_t>(destination.width) * bytes_per_pixel;
    if (source.data == nullptr || destination.data == nullptr || source.width == 0U || source.height == 0U ||
        destination.width == 0U || destination.height == 0U || source.width > INT32_MAX || source.height > INT32_MAX ||
        destination.width > INT32_MAX || destination.height > INT32_MAX ||
        source.pixel_format != destination.pixel_format || source.flags != 0U || destination.flags != 0U ||
        source_row_bytes > UINT32_MAX || destination_row_bytes > UINT32_MAX || source.stride != source_row_bytes ||
        destination.stride < destination_row_bytes ||
        source.size < static_cast<uint64_t>(source.stride) * source.height ||
        destination.size < static_cast<uint64_t>(destination.stride) * destination.height ||
        source.stride > INT32_MAX) {
        return false;
    }
    const uint64_t scratch_row_bytes = static_cast<uint64_t>(destination.width) * 4U;
    if (transform_scratch_ == nullptr || scratch_row_bytes > transform_scratch_size_) {
        return false;
    }

    int32_t scale_x = 0;
    int32_t scale_y = 0;
    if (!ScaleFactor(static_cast<int32_t>(source.width), static_cast<int32_t>(destination.width), scale_x) ||
        !ScaleFactor(static_cast<int32_t>(source.height), static_cast<int32_t>(destination.height), scale_y)) {
        return false;
    }
    lv_draw_image_dsc_t transform{};
    lv_draw_image_dsc_init(&transform);
    transform.scale_x = scale_x;
    transform.scale_y = scale_y;
    transform.pivot = {0, 0};
    transform.rotation = 0;
    transform.antialias = false;

    auto* output = const_cast<uint8_t*>(destination.data);
    const uint32_t rows_per_batch = static_cast<uint32_t>(transform_scratch_size_ / scratch_row_bytes);
    for (uint32_t row = 0U; row < destination.height;) {
        const uint32_t remaining_rows = destination.height - row;
        const uint32_t batch_height = rows_per_batch < remaining_rows ? rows_per_batch : remaining_rows;
        const lv_area_t transform_area{
            .x1 = 0,
            .y1 = static_cast<int32_t>(row),
            .x2 = static_cast<int32_t>(destination.width - 1U),
            .y2 = static_cast<int32_t>(row + batch_height - 1U),
        };
        lv_draw_sw_transform(&transform_area, source.data, static_cast<int32_t>(source.width),
                             static_cast<int32_t>(source.height), static_cast<int32_t>(source.stride), &transform,
                             nullptr, source_format, transform_scratch_);
        for (uint32_t batch_row = 0U; batch_row < batch_height; ++batch_row) {
            const uint8_t* transformed = transform_scratch_ + static_cast<size_t>(batch_row) * destination.width * 4U;
            uint8_t* destination_row = output + static_cast<size_t>(row + batch_row) * destination.stride;
            if (bytes_per_pixel == 4U) {
                lv_memcpy(destination_row, transformed, static_cast<size_t>(destination.width) * 4U);
                continue;
            }
            for (uint32_t x = 0U; x < destination.width; ++x) {
                destination_row[x * 3U + 0U] = transformed[x * 4U + 0U];
                destination_row[x * 3U + 1U] = transformed[x * 4U + 1U];
                destination_row[x * 3U + 2U] = transformed[x * 4U + 2U];
            }
        }
        row += batch_height;
    }
    // LVGL limits scale-only sampling to `(source_extent - 1) * scale / 256`.
    // With a quantized downscale that limit can stop one source pixel before
    // the far edge (625 -> 417 is one example), dropping borders authored on
    // the last row or column. Keep LVGL for the full transform and repair only
    // those two O(width + height) boundaries with endpoint-preserving samples.
    PreserveScaledFarEdges(source, destination, bytes_per_pixel);
    return true;
}

bool LvglSoftwarePixelCompositor::Fill(PixelSurface destination, SurfaceRect rect, uint32_t rgb888, uint8_t opacity) {
    if (!ValidDestinationSurface(destination) || !ValidDestinationRect(rect) || (rgb888 & 0xff000000U) != 0U) {
        return false;
    }
    if (opacity == 0U) {
        return true;
    }
    const SurfaceRect clipped = ClipToSurface(rect, destination);
    if (clipped.width == 0 || clipped.height == 0) {
        return true;
    }
    if (destination.stride > INT32_MAX) {
        return reference_.Fill(destination, rect, rgb888, opacity);
    }

    lv_draw_sw_blend_fill_dsc_t descriptor{};
    descriptor.dest_buf =
        MutablePixelAt(destination, static_cast<uint32_t>(clipped.x), static_cast<uint32_t>(clipped.y));
    descriptor.dest_w = clipped.width;
    descriptor.dest_h = clipped.height;
    descriptor.dest_stride = static_cast<int32_t>(destination.stride);
    descriptor.color = lv_color_make(static_cast<uint8_t>(rgb888 >> 16U), static_cast<uint8_t>(rgb888 >> 8U),
                                     static_cast<uint8_t>(rgb888));
    descriptor.opa = opacity;
    descriptor.relative_area = LocalArea(clipped.width, clipped.height);
    BlendFill(descriptor, destination.format);
    return true;
}

bool LvglSoftwarePixelCompositor::Blit(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                                       SurfaceRect destination_rect, uint8_t opacity) {
    if (!ValidSurface(source) || !ValidDestinationSurface(destination) || !ValidSourceRect(source, source_rect) ||
        !ValidDestinationRect(destination_rect)) {
        return false;
    }
    if (opacity == 0U) {
        return true;
    }
    const SurfaceRect clipped = ClipToSurface(destination_rect, destination);
    if (clipped.width == 0 || clipped.height == 0) {
        return true;
    }
    if (source.stride > INT32_MAX || destination.stride > INT32_MAX) {
        return reference_.Blit(source, source_rect, destination, destination_rect, opacity);
    }

    const bool same_size = source_rect.width == destination_rect.width && source_rect.height == destination_rect.height;
    if (!same_size) {
        return TransformBlit(source, source_rect, destination, destination_rect, clipped, opacity);
    }

    const uint32_t source_x = static_cast<uint32_t>(source_rect.x + clipped.x - destination_rect.x);
    const uint32_t source_y = static_cast<uint32_t>(source_rect.y + clipped.y - destination_rect.y);
    lv_draw_sw_blend_image_dsc_t descriptor{};
    descriptor.dest_buf =
        MutablePixelAt(destination, static_cast<uint32_t>(clipped.x), static_cast<uint32_t>(clipped.y));
    descriptor.dest_w = clipped.width;
    descriptor.dest_h = clipped.height;
    descriptor.dest_stride = static_cast<int32_t>(destination.stride);
    descriptor.src_buf = PixelAt(source, source_x, source_y);
    descriptor.src_stride = static_cast<int32_t>(source.stride);
    descriptor.src_color_format = LvColorFormat(source.format);
    descriptor.opa = opacity;
    descriptor.blend_mode = LV_BLEND_MODE_NORMAL;
    descriptor.relative_area = LocalArea(clipped.width, clipped.height);
    descriptor.src_area = descriptor.relative_area;
    BlendImage(descriptor, destination.format);
    return true;
}

bool LvglSoftwarePixelCompositor::TransformBlit(ConstPixelSurface source, SurfaceRect source_rect,
                                                PixelSurface destination, SurfaceRect destination_rect,
                                                SurfaceRect clipped, uint8_t opacity) {
    const uint64_t scratch_row_bytes = static_cast<uint64_t>(clipped.width) * 4U;
    const int64_t relative_x = static_cast<int64_t>(clipped.x) - destination_rect.x;
    const int64_t relative_y = static_cast<int64_t>(clipped.y) - destination_rect.y;
    if (transform_scratch_ == nullptr || scratch_row_bytes > transform_scratch_size_ || relative_x < 0 ||
        relative_y < 0 || relative_x + clipped.width - 1 > INT32_MAX || relative_y + clipped.height - 1 > INT32_MAX) {
        return reference_.Blit(source, source_rect, destination, destination_rect, opacity);
    }

    int32_t scale_x = 0;
    int32_t scale_y = 0;
    if (!ScaleFactor(source_rect.width, destination_rect.width, scale_x) ||
        !ScaleFactor(source_rect.height, destination_rect.height, scale_y)) {
        return reference_.Blit(source, source_rect, destination, destination_rect, opacity);
    }

    lv_draw_image_dsc_t transform{};
    lv_draw_image_dsc_init(&transform);
    transform.scale_x = scale_x;
    transform.scale_y = scale_y;
    transform.pivot = {0, 0};
    transform.rotation = 0;
    transform.antialias = false;

    const lv_color_format_t source_format = LvColorFormat(source.format);
    const uint8_t* source_pixels =
        PixelAt(source, static_cast<uint32_t>(source_rect.x), static_cast<uint32_t>(source_rect.y));
    const int32_t rows_per_batch = static_cast<int32_t>(transform_scratch_size_ / scratch_row_bytes);
    for (int32_t row = 0; row < clipped.height;) {
        const int32_t remaining_rows = clipped.height - row;
        const int32_t batch_height = rows_per_batch < remaining_rows ? rows_per_batch : remaining_rows;
        const int32_t transformed_y = static_cast<int32_t>(relative_y) + row;
        const lv_area_t transform_area{
            .x1 = static_cast<int32_t>(relative_x),
            .y1 = transformed_y,
            .x2 = static_cast<int32_t>(relative_x) + clipped.width - 1,
            .y2 = transformed_y + batch_height - 1,
        };
        lv_draw_sw_transform(&transform_area, source_pixels, source_rect.width, source_rect.height,
                             static_cast<int32_t>(source.stride), &transform, nullptr, source_format,
                             transform_scratch_);

        lv_draw_sw_blend_image_dsc_t blend{};
        blend.dest_buf =
            MutablePixelAt(destination, static_cast<uint32_t>(clipped.x), static_cast<uint32_t>(clipped.y + row));
        blend.dest_w = clipped.width;
        blend.dest_h = batch_height;
        blend.dest_stride = static_cast<int32_t>(destination.stride);
        blend.opa = opacity;
        blend.blend_mode = LV_BLEND_MODE_NORMAL;
        blend.relative_area = LocalArea(clipped.width, batch_height);
        blend.src_area = blend.relative_area;
        if (source.format == SurfacePixelFormat::kRgb565) {
            blend.src_buf = transform_scratch_;
            blend.src_stride = clipped.width * 2;
            blend.src_color_format = LV_COLOR_FORMAT_RGB565;
        } else {
            blend.src_buf = transform_scratch_;
            blend.src_stride = clipped.width * 4;
            blend.src_color_format =
                source.format == SurfacePixelFormat::kBgr888 ? LV_COLOR_FORMAT_XRGB8888 : LV_COLOR_FORMAT_ARGB8888;
        }
        BlendImage(blend, destination.format);
        row += batch_height;
    }
    return true;
}

}  // namespace micropixel::platform::lvgl
