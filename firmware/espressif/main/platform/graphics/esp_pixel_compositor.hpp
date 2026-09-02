#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "driver/ppa.h"
#include "esp_async_color_convert.h"
#include "esp_err.h"
#include "platform/graphics/pixel_compositor.hpp"

namespace micropixel::platform::graphics {

inline constexpr std::size_t kPpaBlendHistogramBinCount = 8U;

struct HardwarePixelCompositorStats final {
    uint32_t ppa_fills{};
    uint32_t ppa_blends{};
    uint32_t small_blend_candidates{};
    uint32_t ppa_scales{};
    uint32_t dma2d_copies{};
    uint32_t software_fallbacks{};
    uint64_t ppa_blend_pixels{};
    uint64_t ppa_blend_elapsed_us{};
    std::array<uint32_t, kPpaBlendHistogramBinCount> ppa_blend_histogram_calls{};
    std::array<uint64_t, kPpaBlendHistogramBinCount> ppa_blend_histogram_elapsed_us{};
    uint32_t sram_staged_blends{};
    uint32_t sram_staged_failures{};
    uint64_t sram_staged_blend_pixels{};
    uint64_t sram_staged_total_us{};
    uint64_t sram_staged_pack_background_us{};
    uint64_t sram_staged_pack_foreground_us{};
    uint64_t sram_staged_ppa_us{};
    uint64_t sram_staged_copy_back_us{};
};

// Shared ESP32-P4/S31 implementation of the platform PixelCompositor contract.
// Operations are promoted independently, so unsupported ratios, tiny regions,
// alpha/format combinations or unavailable clients retain exact CPU semantics.
class EspPixelCompositor final : public PixelCompositor {
   public:
    explicit EspPixelCompositor(PixelCompositor& software) : software_(software) {}
    EspPixelCompositor(const EspPixelCompositor&) = delete;
    EspPixelCompositor& operator=(const EspPixelCompositor&) = delete;
    ~EspPixelCompositor() override;

    [[nodiscard]] esp_err_t Initialize(uint32_t minimum_hardware_pixels);
    void Release();

    [[nodiscard]] bool Fill(PixelSurface destination, SurfaceRect rect, uint32_t rgb888, uint8_t opacity) override;
    [[nodiscard]] bool Blit(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                            SurfaceRect destination_rect, uint8_t opacity) override;

    [[nodiscard]] HardwarePixelCompositorStats Stats() const { return stats_; }

   private:
    [[nodiscard]] bool TryFill(PixelSurface destination, SurfaceRect rect, uint32_t rgb888);
    [[nodiscard]] bool TryDma2dCopy(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                                    SurfaceRect destination_rect);
    [[nodiscard]] bool TryScale(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                                SurfaceRect destination_rect);
    [[nodiscard]] bool TrySramStagedBlend(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                                          SurfaceRect destination_rect, uint8_t opacity);
    [[nodiscard]] bool TryBlend(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                                SurfaceRect destination_rect, uint8_t opacity);

    PixelCompositor& software_;
    ppa_client_handle_t fill_client_{};
    ppa_client_handle_t scale_client_{};
    ppa_client_handle_t blend_client_{};
    async_color_convert_handle_t dma2d_client_{};
    uint8_t* sram_staged_blend_scratch_{};
    uint32_t minimum_hardware_pixels_{1U};
    HardwarePixelCompositorStats stats_{};
};

}  // namespace micropixel::platform::graphics
