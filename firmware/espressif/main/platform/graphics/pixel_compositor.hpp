#pragma once

#include <cstdint>

namespace micropixel::platform::graphics {

enum class SurfacePixelFormat : uint8_t {
    kBgr888,
    kBgra8888,
    kRgb565,
};

struct SurfaceRect final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

struct ConstPixelSurface final {
    const uint8_t* pixels{};
    uint32_t size{};
    uint32_t width{};
    uint32_t height{};
    uint32_t stride{};
    SurfacePixelFormat format{};
    uint32_t origin_x{};
    uint32_t origin_y{};
};

struct PixelSurface final {
    uint8_t* pixels{};
    uint32_t size{};
    uint32_t width{};
    uint32_t height{};
    uint32_t stride{};
    SurfacePixelFormat format{};
    uint32_t origin_x{};
    uint32_t origin_y{};

    [[nodiscard]] ConstPixelSurface ReadOnly() const {
        return {.pixels = pixels,
                .size = size,
                .width = width,
                .height = height,
                .stride = stride,
                .format = format,
                .origin_x = origin_x,
                .origin_y = origin_y};
    }
};

// Platform-internal pixel boundary. Sources may use every declared format;
// destinations are opaque BGR888 or RGB565 surfaces. Hardware implementations
// may use PPA and DMA2D, while the reference implementation provides identical
// semantics for small regions, unsupported transforms, tests and fallback.
class PixelCompositor {
   public:
    virtual ~PixelCompositor() = default;
    PixelCompositor(const PixelCompositor&) = delete;
    PixelCompositor& operator=(const PixelCompositor&) = delete;

    [[nodiscard]] virtual bool Fill(PixelSurface destination, SurfaceRect rect, uint32_t rgb888, uint8_t opacity) = 0;
    // Rasterizes directly into destination. Implemented as horizontal spans so
    // platform Fill implementations retain their native RGB565 blend path.
    [[nodiscard]] bool RoundedRect(PixelSurface destination, SurfaceRect rect, uint32_t radius, uint32_t fill_rgb888,
                                   uint32_t stroke_rgb888, uint32_t stroke_width, uint8_t opacity);
    [[nodiscard]] virtual bool Blit(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                                    SurfaceRect destination_rect, uint8_t opacity) = 0;

   protected:
    PixelCompositor() = default;
};

class SoftwarePixelCompositor final : public PixelCompositor {
   public:
    [[nodiscard]] bool Fill(PixelSurface destination, SurfaceRect rect, uint32_t rgb888, uint8_t opacity) override;
    [[nodiscard]] bool Blit(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                            SurfaceRect destination_rect, uint8_t opacity) override;
};

}  // namespace micropixel::platform::graphics
