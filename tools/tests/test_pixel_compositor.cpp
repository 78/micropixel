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
    BgraSourceBlendsDirectlyIntoRgb565();
    RejectsMalformedSurfaceStorage();
    return 0;
}
