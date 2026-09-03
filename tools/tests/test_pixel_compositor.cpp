#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

#include "platform/graphics/pixel_compositor.hpp"

namespace graphics = micropixel::platform::graphics;

namespace {

template <size_t Size>
graphics::PixelSurface BgrSurface(std::array<uint8_t, Size>& pixels, uint32_t width, uint32_t height) {
    return {
        .pixels = pixels.data(),
        .size = static_cast<uint32_t>(pixels.size()),
        .width = width,
        .height = height,
        .stride = width * 3U,
        .format = graphics::SurfacePixelFormat::kBgr888,
    };
}

void FillClipsAndUsesCanonicalBgrOrder() {
    graphics::SoftwarePixelCompositor compositor;
    std::array<uint8_t, 3U * 2U * 2U> pixels{};
    graphics::PixelSurface surface = BgrSurface(pixels, 2U, 2U);
    assert(compositor.Fill(surface, {.x = -1, .y = 0, .width = 2, .height = 1}, 0x123456U, 255U));
    assert(pixels[0] == 0x56U);
    assert(pixels[1] == 0x34U);
    assert(pixels[2] == 0x12U);
    for (size_t index = 3U; index < pixels.size(); ++index) {
        assert(pixels[index] == 0U);
    }
}

void FillBlendsWithExactOpacity() {
    graphics::SoftwarePixelCompositor compositor;
    std::array<uint8_t, 3U> pixels{0U, 0U, 0U};
    graphics::PixelSurface surface = BgrSurface(pixels, 1U, 1U);
    assert(compositor.Fill(surface, {.x = 0, .y = 0, .width = 1, .height = 1}, 0xff0000U, 128U));
    assert(pixels[0] == 0U);
    assert(pixels[1] == 0U);
    assert(pixels[2] == 128U);
}

void BgraSourceAlphaMultipliesUniformOpacity() {
    graphics::SoftwarePixelCompositor compositor;
    const std::array<uint8_t, 4U> source_pixels{0U, 0U, 255U, 128U};
    const graphics::ConstPixelSurface source{
        .pixels = source_pixels.data(),
        .size = static_cast<uint32_t>(source_pixels.size()),
        .width = 1U,
        .height = 1U,
        .stride = 4U,
        .format = graphics::SurfacePixelFormat::kBgra8888,
    };
    std::array<uint8_t, 3U> destination_pixels{255U, 0U, 0U};
    graphics::PixelSurface destination = BgrSurface(destination_pixels, 1U, 1U);
    assert(compositor.Blit(source, {.x = 0, .y = 0, .width = 1, .height = 1}, destination,
                           {.x = 0, .y = 0, .width = 1, .height = 1}, 128U));
    assert(destination_pixels[0] == 191U);
    assert(destination_pixels[1] == 0U);
    assert(destination_pixels[2] == 64U);
}

void BlitUsesNearestNeighborAndDestinationClipping() {
    graphics::SoftwarePixelCompositor compositor;
    const std::array<uint8_t, 6U> source_pixels{
        0U, 0U, 255U, 0U, 255U, 0U,
    };
    const graphics::ConstPixelSurface source{
        .pixels = source_pixels.data(),
        .size = static_cast<uint32_t>(source_pixels.size()),
        .width = 2U,
        .height = 1U,
        .stride = 6U,
        .format = graphics::SurfacePixelFormat::kBgr888,
    };
    std::array<uint8_t, 9U> destination_pixels{};
    graphics::PixelSurface destination = BgrSurface(destination_pixels, 3U, 1U);
    assert(compositor.Blit(source, {.x = 0, .y = 0, .width = 2, .height = 1}, destination,
                           {.x = -1, .y = 0, .width = 4, .height = 1}, 255U));
    assert(destination_pixels[2] == 255U);
    assert(destination_pixels[5] == 0U);
    assert(destination_pixels[4] == 255U);
    assert(destination_pixels[8] == 0U);
    assert(destination_pixels[7] == 255U);
}

void Rgb565RoundTripsPrimaryColors() {
    graphics::SoftwarePixelCompositor compositor;
    std::array<uint8_t, 4U> pixels{};
    graphics::PixelSurface surface{
        .pixels = pixels.data(),
        .size = static_cast<uint32_t>(pixels.size()),
        .width = 2U,
        .height = 1U,
        .stride = 4U,
        .format = graphics::SurfacePixelFormat::kRgb565,
    };
    assert(compositor.Fill(surface, {.x = 0, .y = 0, .width = 1, .height = 1}, 0xff0000U, 255U));
    assert(compositor.Fill(surface, {.x = 1, .y = 0, .width = 1, .height = 1}, 0x00ff00U, 255U));
    uint16_t red = 0U;
    uint16_t green = 0U;
    std::memcpy(&red, pixels.data(), sizeof(red));
    std::memcpy(&green, pixels.data() + 2U, sizeof(green));
    assert(red == 0xf800U);
    assert(green == 0x07e0U);
}

void RoundedRectDrawsFillStrokeAndKeepsCornersTransparent() {
    graphics::SoftwarePixelCompositor compositor;
    std::array<uint8_t, 7U * 5U * 3U> pixels{};
    graphics::PixelSurface surface = BgrSurface(pixels, 7U, 5U);
    assert(
        compositor.RoundedRect(surface, {.x = 0, .y = 0, .width = 7, .height = 5}, 2U, 0xff0000U, 0x00ff00U, 1U, 255U));
    const auto red = [&](uint32_t x, uint32_t y) { return pixels[(y * 7U + x) * 3U + 2U]; };
    const auto green = [&](uint32_t x, uint32_t y) { return pixels[(y * 7U + x) * 3U + 1U]; };
    assert(red(0U, 0U) == 0U && green(0U, 0U) == 0U);
    assert(green(1U, 0U) == 255U && green(5U, 0U) == 255U);
    assert(green(0U, 2U) == 255U && red(1U, 2U) == 255U && green(6U, 2U) == 255U);
}

void RoundedRectAlphaBlendsIntoRgb565() {
    graphics::SoftwarePixelCompositor compositor;
    std::array<uint8_t, 3U * 3U * 2U> pixels{};
    graphics::PixelSurface surface{.pixels = pixels.data(),
                                   .size = static_cast<uint32_t>(pixels.size()),
                                   .width = 3U,
                                   .height = 3U,
                                   .stride = 6U,
                                   .format = graphics::SurfacePixelFormat::kRgb565};
    assert(compositor.RoundedRect(surface, {.x = 0, .y = 0, .width = 3, .height = 3}, 1U, 0xff0000U, 0U, 0U, 128U));
    uint16_t center = 0U;
    std::memcpy(&center, pixels.data() + 8U, sizeof(center));
    assert(center == 0x8000U);
}

void BgraSourceBlendsDirectlyIntoRgb565() {
    graphics::SoftwarePixelCompositor compositor;
    const std::array<uint8_t, 4U> source_pixels{0U, 0U, 255U, 128U};
    const graphics::ConstPixelSurface source{
        .pixels = source_pixels.data(),
        .size = static_cast<uint32_t>(source_pixels.size()),
        .width = 1U,
        .height = 1U,
        .stride = 4U,
        .format = graphics::SurfacePixelFormat::kBgra8888,
    };
    std::array<uint8_t, 2U> destination_pixels{};
    graphics::PixelSurface destination{
        .pixels = destination_pixels.data(),
        .size = static_cast<uint32_t>(destination_pixels.size()),
        .width = 1U,
        .height = 1U,
        .stride = 2U,
        .format = graphics::SurfacePixelFormat::kRgb565,
    };
    assert(compositor.Blit(source, {.x = 0, .y = 0, .width = 1, .height = 1}, destination,
                           {.x = 0, .y = 0, .width = 1, .height = 1}, 255U));
    uint16_t packed = 0U;
    std::memcpy(&packed, destination_pixels.data(), sizeof(packed));
    assert(packed == 0x8000U);
}

// Counts Fill calls so the test can assert the straight band is coalesced
// into block fills instead of one span per row.
class CountingCompositor final : public graphics::PixelCompositor {
   public:
    [[nodiscard]] bool Fill(graphics::PixelSurface destination, graphics::SurfaceRect rect, uint32_t rgb888,
                            uint8_t opacity) override {
        ++fills;
        return software_.Fill(destination, rect, rgb888, opacity);
    }
    [[nodiscard]] bool Blit(graphics::ConstPixelSurface source, graphics::SurfaceRect source_rect,
                            graphics::PixelSurface destination, graphics::SurfaceRect destination_rect,
                            uint8_t opacity) override {
        return software_.Blit(source, source_rect, destination, destination_rect, opacity);
    }
    uint32_t fills{};

   private:
    graphics::SoftwarePixelCompositor software_;
};

void RoundedRectCoalescesStraightBandAndClipsToDestination() {
    constexpr uint32_t kSize = 64U;
    constexpr uint32_t kRadius = 8U;
    constexpr uint32_t kStroke = 2U;
    CountingCompositor full;
    std::array<uint8_t, kSize * kSize * 3U> full_pixels{};
    graphics::PixelSurface full_surface = BgrSurface(full_pixels, kSize, kSize);
    assert(full.RoundedRect(full_surface, {.x = 0, .y = 0, .width = kSize, .height = kSize}, kRadius, 0x0000ffU,
                            0x00ff00U, kStroke, 255U));
    // Corner rows stay per-span (<= 3 fills each); the band is 3 block fills.
    assert(full.fills <= 2U * kRadius * 3U + 3U);
    const auto pixel = [&](uint32_t x, uint32_t y) { return &full_pixels[(y * kSize + x) * 3U]; };
    // Band rows: stroke | fill | stroke, identical to the per-row geometry.
    for (uint32_t y = kRadius; y < kSize - kRadius; ++y) {
        assert(pixel(0U, y)[1] == 255U && pixel(kStroke - 1U, y)[1] == 255U);
        assert(pixel(kStroke, y)[0] == 255U && pixel(kSize - kStroke - 1U, y)[0] == 255U);
        assert(pixel(kSize - kStroke, y)[1] == 255U && pixel(kSize - 1U, y)[1] == 255U);
    }
    // Corners remain transparent, the stroke runs along the top edge.
    assert(pixel(0U, 0U)[1] == 0U && pixel(0U, 0U)[0] == 0U);
    assert(pixel(kSize / 2U, 0U)[1] == 255U);

    // Rendering through a small window at an offset must reproduce exactly
    // the corresponding region of the full render, including a window that
    // starts above the rect and one that only covers the straight band.
    const struct {
        int32_t x;
        int32_t y;
        uint32_t width;
        uint32_t height;
    } windows[] = {
        {3, 5, 20U, 17U},
        {40, 30, 30U, 9U},
        {-4, -6, 24U, 20U},
        {50, 50, 20U, 20U},
    };
    for (const auto& window : windows) {
        CountingCompositor windowed;
        std::array<uint8_t, 30U * 20U * 3U> window_pixels{};
        graphics::PixelSurface window_surface = BgrSurface(window_pixels, window.width, window.height);
        assert(windowed.RoundedRect(window_surface, {.x = -window.x, .y = -window.y, .width = kSize, .height = kSize},
                                    kRadius, 0x0000ffU, 0x00ff00U, kStroke, 255U));
        // Rows outside the window are never rasterized.
        assert(windowed.fills <= (window.height + 1U) * 3U);
        for (uint32_t wy = 0U; wy < window.height; ++wy) {
            for (uint32_t wx = 0U; wx < window.width; ++wx) {
                const int32_t fx = window.x + static_cast<int32_t>(wx);
                const int32_t fy = window.y + static_cast<int32_t>(wy);
                const uint8_t* actual = &window_pixels[(wy * window.width + wx) * 3U];
                if (fx < 0 || fy < 0 || fx >= static_cast<int32_t>(kSize) || fy >= static_cast<int32_t>(kSize)) {
                    assert(actual[0] == 0U && actual[1] == 0U && actual[2] == 0U);
                    continue;
                }
                const uint8_t* expected = pixel(static_cast<uint32_t>(fx), static_cast<uint32_t>(fy));
                assert(std::memcmp(actual, expected, 3U) == 0);
            }
        }
    }
}

void SameSizeBgraBlitMatchesReferenceBlend() {
    graphics::SoftwarePixelCompositor compositor;
    constexpr uint32_t kWidth = 5U;
    constexpr uint32_t kHeight = 4U;
    std::array<uint8_t, kWidth * kHeight * 4U> source_pixels{};
    std::array<uint8_t, kWidth * kHeight * 3U> destination_pixels{};
    uint32_t seed = 12345U;
    const auto next = [&seed]() {
        seed = seed * 1103515245U + 12345U;
        return static_cast<uint8_t>(seed >> 16U);
    };
    for (uint8_t& value : source_pixels) {
        value = next();
    }
    // Force the three alpha classes the fast path distinguishes.
    source_pixels[3U] = 0U;
    source_pixels[7U] = 255U;
    source_pixels[11U] = 128U;
    for (uint8_t& value : destination_pixels) {
        value = next();
    }
    std::array<uint8_t, kWidth * kHeight * 3U> expected = destination_pixels;
    for (uint32_t index = 0U; index < kWidth * kHeight; ++index) {
        const uint8_t* src = &source_pixels[index * 4U];
        uint8_t* dst = &expected[index * 3U];
        const uint32_t alpha = src[3];
        if (alpha == 0U) {
            continue;
        }
        for (uint32_t channel = 0U; channel < 3U; ++channel) {
            dst[channel] = static_cast<uint8_t>((src[channel] * alpha + dst[channel] * (255U - alpha) + 127U) / 255U);
        }
    }
    const graphics::ConstPixelSurface source{
        .pixels = source_pixels.data(),
        .size = static_cast<uint32_t>(source_pixels.size()),
        .width = kWidth,
        .height = kHeight,
        .stride = kWidth * 4U,
        .format = graphics::SurfacePixelFormat::kBgra8888,
    };
    graphics::PixelSurface destination = BgrSurface(destination_pixels, kWidth, kHeight);
    assert(compositor.Blit(source, {.x = 0, .y = 0, .width = kWidth, .height = kHeight}, destination,
                           {.x = 0, .y = 0, .width = kWidth, .height = kHeight}, 255U));
    assert(destination_pixels == expected);

    // Partially clipped placement reads the matching source sub-rectangle.
    std::array<uint8_t, 3U * 2U * 3U> small_pixels{};
    graphics::PixelSurface small = BgrSurface(small_pixels, 3U, 2U);
    assert(compositor.Blit(source, {.x = 1, .y = 1, .width = 3, .height = 2}, small,
                           {.x = -1, .y = -1, .width = 3, .height = 2}, 255U));
    // small(0,0) <- source(2,2), fully opaque pixels copy their colour.
    const uint8_t* src = &source_pixels[(2U * kWidth + 2U) * 4U];
    const uint8_t alpha = src[3];
    const uint8_t expected_blue = static_cast<uint8_t>((src[0] * alpha + 0U * (255U - alpha) + 127U) / 255U);
    assert(small_pixels[0] == expected_blue);
    assert(small_pixels[(1U * 3U + 2U) * 3U] == 0U);  // outside the rect, untouched
}

void RejectsMalformedSurfaceStorage() {
    graphics::SoftwarePixelCompositor compositor;
    std::array<uint8_t, 3U> pixels{};
    graphics::PixelSurface malformed = BgrSurface(pixels, 1U, 1U);
    malformed.stride = 2U;
    assert(!compositor.Fill(malformed, {.x = 0, .y = 0, .width = 1, .height = 1}, 0U, 255U));

    std::array<uint8_t, 4U> rgb565_pixels{};
    graphics::PixelSurface malformed_rgb565{
        .pixels = rgb565_pixels.data(),
        .size = static_cast<uint32_t>(rgb565_pixels.size()),
        .width = 2U,
        .height = 1U,
        .stride = 3U,
        .format = graphics::SurfacePixelFormat::kRgb565,
    };
    assert(!compositor.Fill(malformed_rgb565, {.x = 0, .y = 0, .width = 2, .height = 1}, 0U, 255U));

    std::array<uint8_t, 4U> transparent_pixels{};
    graphics::PixelSurface transparent{
        .pixels = transparent_pixels.data(),
        .size = static_cast<uint32_t>(transparent_pixels.size()),
        .width = 1U,
        .height = 1U,
        .stride = 4U,
        .format = graphics::SurfacePixelFormat::kBgra8888,
    };
    assert(!compositor.Fill(transparent, {.x = 0, .y = 0, .width = 1, .height = 1}, 0U, 255U));
}

}  // namespace

int main() {
    FillClipsAndUsesCanonicalBgrOrder();
    FillBlendsWithExactOpacity();
    BgraSourceAlphaMultipliesUniformOpacity();
    BlitUsesNearestNeighborAndDestinationClipping();
    Rgb565RoundTripsPrimaryColors();
    RoundedRectDrawsFillStrokeAndKeepsCornersTransparent();
    RoundedRectAlphaBlendsIntoRgb565();
    RoundedRectCoalescesStraightBandAndClipsToDestination();
    SameSizeBgraBlitMatchesReferenceBlend();
    BgraSourceBlendsDirectlyIntoRgb565();
    RejectsMalformedSurfaceStorage();
    return 0;
}
