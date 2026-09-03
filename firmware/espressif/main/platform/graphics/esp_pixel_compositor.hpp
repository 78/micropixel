#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "driver/ppa.h"
#include "esp_async_color_convert.h"
#include "esp_err.h"
#include "platform/graphics/dma2d_copy_engine.hpp"
#include "platform/graphics/pixel_compositor.hpp"

namespace micropixel::platform::graphics {

inline constexpr std::size_t kPpaBlendHistogramBinCount = 8U;

struct HardwarePixelCompositorStats final {
    uint32_t ppa_fills{};
    uint32_t ppa_blends{};
    uint32_t small_blend_candidates{};
    uint32_t ppa_scales{};
    uint32_t dma2d_copies{};
    // Linked DMA2D transactions issued for batched copies (dma2d_copies counts
    // the individual blocks they carried).
    uint32_t dma2d_batches{};
    uint32_t software_fallbacks{};
    uint64_t ppa_blend_pixels{};
    uint64_t ppa_blend_elapsed_us{};
    // Wall time spent inside each engine, so per-frame render cost can be
    // attributed to fills, copies, blends or CPU fallbacks.
    uint64_t ppa_fill_elapsed_us{};
    uint64_t dma2d_elapsed_us{};
    uint64_t software_elapsed_us{};
    std::array<uint32_t, kPpaBlendHistogramBinCount> ppa_blend_histogram_calls{};
    std::array<uint64_t, kPpaBlendHistogramBinCount> ppa_blend_histogram_elapsed_us{};
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
    // While a batch is open, opaque BGR888 copies are queued and issued as one
    // linked DMA2D transaction. Any other operation (fill, blend, scale, CPU
    // fallback) flushes the queue first, so call order is preserved exactly.
    void BeginBatch() override;
    [[nodiscard]] bool EndBatch() override;

    [[nodiscard]] HardwarePixelCompositorStats Stats() const { return stats_; }

    // Direct DMA2D engine when enabled by Kconfig; nullptr otherwise.
    [[nodiscard]] Dma2dCopyEngine* DirectCopyEngine() { return direct_copy_.Ready() ? &direct_copy_ : nullptr; }

   private:
    [[nodiscard]] bool TryFill(PixelSurface destination, SurfaceRect rect, uint32_t rgb888);
    [[nodiscard]] bool Dma2dCopyEligible(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                                         SurfaceRect destination_rect) const;
    [[nodiscard]] bool TryDma2dCopy(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                                    SurfaceRect destination_rect);
    [[nodiscard]] bool FlushPendingCopies();
    [[nodiscard]] bool TryScale(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                                SurfaceRect destination_rect);
    [[nodiscard]] bool TryBlend(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                                SurfaceRect destination_rect, uint8_t opacity);

    PixelCompositor& software_;
    ppa_client_handle_t fill_client_{};
    ppa_client_handle_t scale_client_{};
    ppa_client_handle_t blend_client_{};
    async_color_convert_handle_t dma2d_client_{};
    Dma2dCopyEngine direct_copy_{};
    uint32_t minimum_hardware_pixels_{1U};
    HardwarePixelCompositorStats stats_{};
    std::array<Dma2dCopyBlock, Dma2dCopyEngine::kMaxBlocks> pending_copies_{};
    std::size_t pending_copy_count_{};
    bool batching_{};
    bool batch_failed_{};
};

}  // namespace micropixel::platform::graphics
