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
    for (uint32_t y = 0U; y < height; ++y) {
        const uint32_t outer_inset = RoundedInset(radius, height, y);
        const int32_t outer_x = rect.x + static_cast<int32_t>(outer_inset);
        const int32_t outer_width = static_cast<int32_t>(width - outer_inset * 2U);
        if (stroke_width == 0U) {
            if (!Fill(destination,
                      {.x = outer_x, .y = rect.y + static_cast<int32_t>(y), .width = outer_width, .height = 1},
                      fill_rgb888, opacity)) {
                return false;
            }
            continue;
        }
        const bool inner_row = has_inner && y >= stroke_width && y < height - stroke_width;
        if (!inner_row) {
            if (!Fill(destination,
                      {.x = outer_x, .y = rect.y + static_cast<int32_t>(y), .width = outer_width, .height = 1},
                      stroke_rgb888, opacity)) {
                return false;
            }
            continue;
        }
        const uint32_t inner_y = y - stroke_width;
        const uint32_t inner_inset = RoundedInset(inner_radius, inner_height, inner_y);
        const int32_t inner_x = rect.x + static_cast<int32_t>(stroke_width + inner_inset);
        const int32_t inner_span = static_cast<int32_t>(inner_width - inner_inset * 2U);
        const int32_t left_span = inner_x - outer_x;
        const int32_t right_x = inner_x + inner_span;
        const int32_t right_span = outer_x + outer_width - right_x;
        const int32_t row_y = rect.y + static_cast<int32_t>(y);
        if ((left_span > 0 &&
             !Fill(destination, {.x = outer_x, .y = row_y, .width = left_span, .height = 1}, stroke_rgb888, opacity)) ||
            !Fill(destination, {.x = inner_x, .y = row_y, .width = inner_span, .height = 1}, fill_rgb888, opacity) ||
            (right_span > 0 && !Fill(destination, {.x = right_x, .y = row_y, .width = right_span, .height = 1},
                                     stroke_rgb888, opacity))) {
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
    for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) {
        for (int32_t x = clipped.x; x < clipped.x + clipped.width; ++x) {
            uint8_t* pixel = MutablePixelAt(destination, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
            const Rgb result =
                opacity == 255U ? foreground : Blend(foreground, ReadRgb(pixel, destination.format), opacity);
            WriteRgb(pixel, destination.format, result);
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
