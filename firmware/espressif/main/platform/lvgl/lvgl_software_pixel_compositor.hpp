#pragma once

#include <cstdint>

#include "device/contracts/graphics.hpp"
#include "platform/graphics/pixel_compositor.hpp"

namespace micropixel::platform::lvgl {

// LVGL-backed CPU implementation used by the App Surface compositor. Scaling
// consumes a caller-owned, fixed-capacity row-batch scratch buffer and is
// deliberately non-reentrant, matching the single display-task render model.
class LvglSoftwarePixelCompositor final : public graphics::PixelCompositor {
   public:
    void SetTransformScratch(uint8_t* pixels, uint32_t size);
    [[nodiscard]] bool ScaleBitmap(const device::BitmapView& source, const device::BitmapView& destination);

    [[nodiscard]] bool Fill(graphics::PixelSurface destination, graphics::SurfaceRect rect, uint32_t rgb888,
                            uint8_t opacity) override;
    [[nodiscard]] bool Blit(graphics::ConstPixelSurface source, graphics::SurfaceRect source_rect,
                            graphics::PixelSurface destination, graphics::SurfaceRect destination_rect,
                            uint8_t opacity) override;

   private:
    [[nodiscard]] bool TransformBlit(graphics::ConstPixelSurface source, graphics::SurfaceRect source_rect,
                                     graphics::PixelSurface destination, graphics::SurfaceRect destination_rect,
                                     graphics::SurfaceRect clipped, uint8_t opacity);

    graphics::SoftwarePixelCompositor reference_{};
    uint8_t* transform_scratch_{};
    uint32_t transform_scratch_size_{};
};

}  // namespace micropixel::platform::lvgl
