#include "platform/graphics/pixel_compositor.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace micropixel::platform::graphics {
namespace {

struct Rgb final {
    uint8_t red{};
    uint8_t green{};
    uint8_t blue{};
};

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

uint8_t Expand5(uint16_t value) { return static_cast<uint8_t>((value * 255U + 15U) / 31U); }

uint8_t Expand6(uint16_t value) { return static_cast<uint8_t>((value * 255U + 31U) / 63U); }

uint16_t Compress5(uint8_t value) { return static_cast<uint16_t>((value * 31U + 127U) / 255U); }

uint16_t Compress6(uint8_t value) { return static_cast<uint16_t>((value * 63U + 127U) / 255U); }

Rgb ReadRgb(const uint8_t* pixel, SurfacePixelFormat format) {
    if (format == SurfacePixelFormat::kRgb565) {
        uint16_t packed = 0U;
        std::memcpy(&packed, pixel, sizeof(packed));
        return {
            .red = Expand5(static_cast<uint16_t>((packed >> 11U) & 0x1fU)),
            .green = Expand6(static_cast<uint16_t>((packed >> 5U) & 0x3fU)),
            .blue = Expand5(static_cast<uint16_t>(packed & 0x1fU)),
        };
    }
    return {.red = pixel[2], .green = pixel[1], .blue = pixel[0]};
}

uint8_t ReadAlpha(const uint8_t* pixel, SurfacePixelFormat format) {
    return format == SurfacePixelFormat::kBgra8888 ? pixel[3] : 255U;
}

void WriteRgb(uint8_t* pixel, SurfacePixelFormat format, Rgb color) {
    if (format == SurfacePixelFormat::kRgb565) {
        const uint16_t packed = static_cast<uint16_t>((Compress5(color.red) << 11U) | (Compress6(color.green) << 5U) |
                                                      Compress5(color.blue));
        std::memcpy(pixel, &packed, sizeof(packed));
        return;
    }
    pixel[0] = color.blue;
    pixel[1] = color.green;
    pixel[2] = color.red;
    if (format == SurfacePixelFormat::kBgra8888) {
        pixel[3] = 255U;
    }
}

uint8_t BlendChannel(uint8_t foreground, uint8_t background, uint8_t alpha) {
    const uint32_t inverse = 255U - alpha;
    return static_cast<uint8_t>(
        (static_cast<uint32_t>(foreground) * alpha + static_cast<uint32_t>(background) * inverse + 127U) / 255U);
}

Rgb Blend(Rgb foreground, Rgb background, uint8_t alpha) {
    return {
        .red = BlendChannel(foreground.red, background.red, alpha),
        .green = BlendChannel(foreground.green, background.green, alpha),
        .blue = BlendChannel(foreground.blue, background.blue, alpha),
    };
}

Rgb RgbFromValue(uint32_t rgb888) {
    return {
        .red = static_cast<uint8_t>(rgb888 >> 16U),
        .green = static_cast<uint8_t>(rgb888 >> 8U),
        .blue = static_cast<uint8_t>(rgb888),
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

uint32_t IntegerSquareRoot(uint64_t value) {
    uint64_t result = 0U;
    uint64_t bit = 1ULL << 62U;
    while (bit > value) {
        bit >>= 2U;
    }
    while (bit != 0U) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1U) + bit;
        } else {
            result >>= 1U;
        }
        bit >>= 2U;
    }
    return static_cast<uint32_t>(result);
}

uint32_t RoundedInset(uint32_t radius, uint32_t height, uint32_t y) {
    if (radius == 0U || (y >= radius && y < height - radius)) {
        return 0U;
    }
    const uint32_t corner_y = y < radius ? y : height - 1U - y;
    const int64_t doubled_y = static_cast<int64_t>(corner_y) * 2 + 1 - static_cast<int64_t>(radius) * 2;
    const uint64_t diameter_squared = static_cast<uint64_t>(radius) * radius * 4U;
    const uint64_t y_squared = static_cast<uint64_t>(doubled_y * doubled_y);
    const uint32_t horizontal = IntegerSquareRoot(diameter_squared - y_squared);
    return (radius * 2U - horizontal) / 2U;
}

// Unscaled blit: rows map one to one, so the source position is plain pointer
// arithmetic instead of the per-pixel 64-bit scaling division of the general
// path. The BGRA8888 -> BGR888 sprite case (the common ARGB atlas draw) and
// the opaque BGR888 copy have dedicated loops; everything else shares the
// format-dispatching per-pixel blend with the same rounding as the general
// path.
void BlitSameSize(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                  SurfaceRect destination_rect, SurfaceRect clipped, uint8_t opacity) {
    const uint32_t source_x = static_cast<uint32_t>(source_rect.x + (clipped.x - destination_rect.x));
    const uint32_t source_y = static_cast<uint32_t>(source_rect.y + (clipped.y - destination_rect.y));
    const uint32_t width = static_cast<uint32_t>(clipped.width);
    const uint32_t height = static_cast<uint32_t>(clipped.height);
    const uint8_t* source_row = PixelAt(source, source_x, source_y);
    uint8_t* destination_row =
        MutablePixelAt(destination, static_cast<uint32_t>(clipped.x), static_cast<uint32_t>(clipped.y));

    if (source.format == SurfacePixelFormat::kBgr888 && destination.format == SurfacePixelFormat::kBgr888 &&
        opacity == 255U) {
        const size_t row_bytes = static_cast<size_t>(width) * 3U;
        for (uint32_t y = 0U; y < height; ++y) {
            std::memcpy(destination_row, source_row, row_bytes);
            source_row += source.stride;
            destination_row += destination.stride;
        }
        return;
    }
    if (source.format == SurfacePixelFormat::kBgra8888 && destination.format == SurfacePixelFormat::kBgr888) {
        for (uint32_t y = 0U; y < height; ++y) {
            const uint8_t* src = source_row;
            uint8_t* dst = destination_row;
            for (uint32_t x = 0U; x < width; ++x, src += 4U, dst += 3U) {
                const uint32_t alpha =
                    opacity == 255U ? src[3] : (static_cast<uint32_t>(src[3]) * opacity + 127U) / 255U;
                if (alpha == 0U) {
                    continue;
                }
                if (alpha == 255U) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    continue;
                }
                const uint32_t inverse = 255U - alpha;
                dst[0] = static_cast<uint8_t>((src[0] * alpha + dst[0] * inverse + 127U) / 255U);
                dst[1] = static_cast<uint8_t>((src[1] * alpha + dst[1] * inverse + 127U) / 255U);
                dst[2] = static_cast<uint8_t>((src[2] * alpha + dst[2] * inverse + 127U) / 255U);
            }
            source_row += source.stride;
            destination_row += destination.stride;
        }
        return;
    }
    const uint32_t source_bytes = BytesPerPixel(source.format);
    const uint32_t destination_bytes = BytesPerPixel(destination.format);
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t* src = source_row;
        uint8_t* dst = destination_row;
        for (uint32_t x = 0U; x < width; ++x, src += source_bytes, dst += destination_bytes) {
            const uint8_t source_alpha = ReadAlpha(src, source.format);
            const uint8_t effective_alpha =
                static_cast<uint8_t>((static_cast<uint32_t>(source_alpha) * opacity + 127U) / 255U);
            if (effective_alpha == 0U) {
                continue;
            }
            const Rgb foreground = ReadRgb(src, source.format);
            const Rgb result = effective_alpha == 255U
                                   ? foreground
                                   : Blend(foreground, ReadRgb(dst, destination.format), effective_alpha);
            WriteRgb(dst, destination.format, result);
        }
        source_row += source.stride;
        destination_row += destination.stride;
    }
}

}  // namespace

bool PixelCompositor::RoundedRect(PixelSurface destination, SurfaceRect rect, uint32_t radius, uint32_t fill_rgb888,
                                  uint32_t stroke_rgb888, uint32_t stroke_width, uint8_t opacity) {
    if (!ValidDestinationSurface(destination) || !ValidDestinationRect(rect) || (fill_rgb888 & 0xff000000U) != 0U ||
        (stroke_rgb888 & 0xff000000U) != 0U) {
        return false;
    }
    const uint32_t short_edge = static_cast<uint32_t>(rect.width < rect.height ? rect.width : rect.height);
    radius = radius < short_edge / 2U ? radius : short_edge / 2U;
    stroke_width = stroke_width < short_edge / 2U ? stroke_width : short_edge / 2U;
    if (opacity == 0U) {
        return true;
    }
    const uint32_t width = static_cast<uint32_t>(rect.width);
    const uint32_t height = static_cast<uint32_t>(rect.height);
    const bool has_inner = stroke_width == 0U || (stroke_width * 2U < width && stroke_width * 2U < height);
    const uint32_t inner_width = has_inner ? width - stroke_width * 2U : 0U;
    const uint32_t inner_height = has_inner ? height - stroke_width * 2U : 0U;
    const uint32_t inner_radius = radius > stroke_width ? radius - stroke_width : 0U;

    // Only the rows that land inside the destination are rasterized. The
    // compositor replays a node clipped to each damage region, so a large
    // board is usually visible through a small window and the rest of its
    // rows would be clipped away by every Fill anyway.
    const int64_t first_visible = rect.y < 0 ? -static_cast<int64_t>(rect.y) : 0;
    const int64_t last_visible_exclusive =
        static_cast<int64_t>(destination.height) - rect.y < static_cast<int64_t>(height)
            ? static_cast<int64_t>(destination.height) - rect.y
            : static_cast<int64_t>(height);
    if (first_visible >= last_visible_exclusive) {
        return true;
    }
    const uint32_t visible_top = static_cast<uint32_t>(first_visible);
    const uint32_t visible_bottom = static_cast<uint32_t>(last_visible_exclusive);

    // Rows between the corner arcs have no inset on either the outer edge or
    // the inner fill; they are emitted as up to three block fills instead of
    // one span per row so hardware fill engines see one large rectangle.
    const uint32_t band_margin = radius > stroke_width ? radius : stroke_width;
    const uint32_t band_top = band_margin < height ? band_margin : height;
    const uint32_t band_bottom = height > band_margin ? height - band_margin : 0U;
    const bool has_band = band_top < band_bottom;

    const auto draw_row = [&](uint32_t y) -> bool {
        const uint32_t outer_inset = RoundedInset(radius, height, y);
        const int32_t outer_x = rect.x + static_cast<int32_t>(outer_inset);
        const int32_t outer_width = static_cast<int32_t>(width - outer_inset * 2U);
        const int32_t row_y = rect.y + static_cast<int32_t>(y);
        if (stroke_width == 0U) {
            return Fill(destination, {.x = outer_x, .y = row_y, .width = outer_width, .height = 1}, fill_rgb888,
                        opacity);
        }
        const bool inner_row = has_inner && y >= stroke_width && y < height - stroke_width;
        if (!inner_row) {
            return Fill(destination, {.x = outer_x, .y = row_y, .width = outer_width, .height = 1}, stroke_rgb888,
                        opacity);
        }
        const uint32_t inner_y = y - stroke_width;
        const uint32_t inner_inset = RoundedInset(inner_radius, inner_height, inner_y);
        const int32_t inner_x = rect.x + static_cast<int32_t>(stroke_width + inner_inset);
        const int32_t inner_span = static_cast<int32_t>(inner_width - inner_inset * 2U);
        const int32_t left_span = inner_x - outer_x;
        const int32_t right_x = inner_x + inner_span;
        const int32_t right_span = outer_x + outer_width - right_x;
        return (left_span <= 0 || Fill(destination, {.x = outer_x, .y = row_y, .width = left_span, .height = 1},
                                       stroke_rgb888, opacity)) &&
               Fill(destination, {.x = inner_x, .y = row_y, .width = inner_span, .height = 1}, fill_rgb888, opacity) &&
               (right_span <= 0 || Fill(destination, {.x = right_x, .y = row_y, .width = right_span, .height = 1},
                                        stroke_rgb888, opacity));
    };

    const auto draw_band = [&](uint32_t top, uint32_t bottom) -> bool {
        const int32_t band_y = rect.y + static_cast<int32_t>(top);
        const int32_t band_height = static_cast<int32_t>(bottom - top);
        if (stroke_width == 0U) {
            return Fill(destination, {.x = rect.x, .y = band_y, .width = rect.width, .height = band_height},
                        fill_rgb888, opacity);
        }
        if (!has_inner) {
            return Fill(destination, {.x = rect.x, .y = band_y, .width = rect.width, .height = band_height},
                        stroke_rgb888, opacity);
        }
        const int32_t stroke_span = static_cast<int32_t>(stroke_width);
        const int32_t inner_x = rect.x + stroke_span;
        const int32_t right_x = inner_x + static_cast<int32_t>(inner_width);
        return Fill(destination, {.x = rect.x, .y = band_y, .width = stroke_span, .height = band_height}, stroke_rgb888,
                    opacity) &&
               Fill(destination,
                    {.x = inner_x, .y = band_y, .width = static_cast<int32_t>(inner_width), .height = band_height},
                    fill_rgb888, opacity) &&
               Fill(destination, {.x = right_x, .y = band_y, .width = stroke_span, .height = band_height},
                    stroke_rgb888, opacity);
    };

    if (!has_band) {
        for (uint32_t y = visible_top; y < visible_bottom; ++y) {
            if (!draw_row(y)) {
                return false;
            }
        }
        return true;
    }
    const uint32_t top_rows_end = band_top < visible_bottom ? band_top : visible_bottom;
    for (uint32_t y = visible_top; y < top_rows_end; ++y) {
        if (!draw_row(y)) {
            return false;
        }
    }
    const uint32_t visible_band_top = band_top > visible_top ? band_top : visible_top;
    const uint32_t visible_band_bottom = band_bottom < visible_bottom ? band_bottom : visible_bottom;
    if (visible_band_top < visible_band_bottom && !draw_band(visible_band_top, visible_band_bottom)) {
        return false;
    }
    const uint32_t bottom_rows_start = band_bottom > visible_top ? band_bottom : visible_top;
    for (uint32_t y = bottom_rows_start; y < visible_bottom; ++y) {
        if (!draw_row(y)) {
            return false;
        }
    }
    return true;
}

bool SoftwarePixelCompositor::Fill(PixelSurface destination, SurfaceRect rect, uint32_t rgb888, uint8_t opacity) {
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
    const Rgb foreground = RgbFromValue(rgb888);
    if (opacity == 255U) {
        // Opaque fill: rasterize the first row once, then replicate it. The
        // row copy runs at memcpy speed, which is what makes a CPU fill the
        // right choice for the many small rectangles a scene produces.
        uint8_t* first_row =
            MutablePixelAt(destination, static_cast<uint32_t>(clipped.x), static_cast<uint32_t>(clipped.y));
        const size_t bytes_per_pixel = BytesPerPixel(destination.format);
        const size_t row_bytes = static_cast<size_t>(clipped.width) * bytes_per_pixel;
        uint8_t* pixel = first_row;
        for (int32_t x = 0; x < clipped.width; ++x) {
            WriteRgb(pixel, destination.format, foreground);
            pixel += bytes_per_pixel;
        }
        for (int32_t y = 1; y < clipped.height; ++y) {
            std::memcpy(first_row + static_cast<size_t>(y) * destination.stride, first_row, row_bytes);
        }
        return true;
    }
    for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) {
        for (int32_t x = clipped.x; x < clipped.x + clipped.width; ++x) {
            uint8_t* pixel = MutablePixelAt(destination, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
            WriteRgb(pixel, destination.format, Blend(foreground, ReadRgb(pixel, destination.format), opacity));
        }
    }
    return true;
}

bool SoftwarePixelCompositor::Blit(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
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
    if (source_rect.width == destination_rect.width && source_rect.height == destination_rect.height) {
        BlitSameSize(source, source_rect, destination, destination_rect, clipped, opacity);
        return true;
    }
    for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) {
        const int64_t relative_y = static_cast<int64_t>(y) - destination_rect.y;
        const uint32_t source_y = static_cast<uint32_t>(
            source_rect.y + relative_y * source_rect.height / static_cast<int64_t>(destination_rect.height));
        for (int32_t x = clipped.x; x < clipped.x + clipped.width; ++x) {
            const int64_t relative_x = static_cast<int64_t>(x) - destination_rect.x;
            const uint32_t source_x = static_cast<uint32_t>(
                source_rect.x + relative_x * source_rect.width / static_cast<int64_t>(destination_rect.width));
            const uint8_t* source_pixel = PixelAt(source, source_x, source_y);
            uint8_t* destination_pixel =
                MutablePixelAt(destination, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
            const uint8_t source_alpha = ReadAlpha(source_pixel, source.format);
            const uint8_t effective_alpha =
                static_cast<uint8_t>((static_cast<uint32_t>(source_alpha) * opacity + 127U) / 255U);
            if (effective_alpha == 0U) {
                continue;
            }
            const Rgb foreground = ReadRgb(source_pixel, source.format);
            const Rgb result = effective_alpha == 255U
                                   ? foreground
                                   : Blend(foreground, ReadRgb(destination_pixel, destination.format), effective_alpha);
            WriteRgb(destination_pixel, destination.format, result);
        }
    }
    return true;
}

}  // namespace micropixel::platform::graphics
