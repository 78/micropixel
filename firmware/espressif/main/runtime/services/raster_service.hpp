#ifndef MICROPIXEL_RUNTIME_SERVICES_RASTER_SERVICE_HPP
#define MICROPIXEL_RUNTIME_SERVICES_RASTER_SERVICE_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"
#include "runtime/graphics/raster_kernels.hpp"
#include "runtime/services/direct_surface_service.hpp"
#include "runtime/services/service_result.hpp"

namespace micropixel::runtime {

// Per-session INDEX8 resources: texture, palette and warp-map slots, each a
// directly indexed uint8 ID whose metadata table grows only during upload.
// Palettes prefer internal SRAM; textures and warp maps stay in PSRAM unless
// the board opt-in is on. Rendering never allocates. Failed replacements
// preserve the previous resource. Shutdown releases all pixels and metadata.
class RasterService final {
   public:
    explicit RasterService(bool enabled);
    RasterService(const RasterService&) = delete;
    RasterService& operator=(const RasterService&) = delete;
    ~RasterService();

    void BindGuestMemory(const GuestMemoryAccess& access) { memory_ = access; }

    [[nodiscard]] bool available() const { return enabled_; }  // NOLINT(readability-identifier-naming)

    [[nodiscard]] ServiceResult<void> UploadTexture(const micropixel_raster_texture_upload_request_t& request);
    [[nodiscard]] ServiceResult<void> UploadPalette(const micropixel_raster_palette_upload_request_t& request);
    [[nodiscard]] ServiceResult<void> UploadWarp(const micropixel_raster_warp_upload_request_t& request);
    // `surfaces` owns the target buffer and vetoes one the display still reads.
    [[nodiscard]] ServiceResult<void> Submit(const uint8_t* bytes, uint32_t length,
                                             const DirectSurfaceService& surfaces,
                                             raster::TextureResolver resolve = nullptr, void* context = nullptr);

    void Shutdown();

   private:
    // Slot metadata array that grows to cover the highest slot ever uploaded.
    template <typename Entry>
    struct SlotTable final {
        Entry* entries{};
        uint32_t capacity{};
    };
    // `prefer_internal` tries internal SRAM first (palettes: one random lookup
    // per output pixel). Textures and warp maps stay on the PSRAM path unless
    // CONFIG_MICROPIXEL_RASTER_POOL_INTERNAL_SRAM is on. Either way, failure
    // falls back to PSRAM.
    [[nodiscard]] static uint8_t* Allocate(uint32_t bytes, bool prefer_internal = false);
    // Validated Host pointer for a Guest range, or nullptr.
    [[nodiscard]] const uint8_t* ResolveGuest(uint32_t offset, uint32_t length) const;
    // Grows `table` so `index` is addressable, or returns false without
    // touching it when the larger table cannot be allocated.
    template <typename Entry>
    [[nodiscard]] static bool Reserve(SlotTable<Entry>& table, uint32_t index);
    // Palette entries are stored in the byte order of the buffers they are
    // copied into; converts every slot that is not in `byte_swapped` order.
    void MatchPaletteByteOrder(bool byte_swapped);
    // Recomputes the non-skip spans of rows row0..row0+row_count-1.
    static void RefreshWarpSpans(raster::WarpMap& map, uint32_t row0, uint32_t row_count);
    [[nodiscard]] raster::Resources ResourcesView() const;

    bool enabled_{};
    GuestMemoryAccess memory_{};
    SlotTable<raster::Texture> textures_{};
    SlotTable<raster::Palette> palettes_{};
    SlotTable<raster::WarpMap> warps_{};

    // Periodic kernel telemetry (CONFIG_MICROPIXEL_APP_SURFACE_TELEMETRY_LOG).
    static constexpr uint32_t kTelemetrySubmits = 240U;
    uint32_t telemetry_submits_{};
    uint32_t telemetry_records_{};
    uint32_t telemetry_bytes_{};
    uint64_t telemetry_execute_us_{};
    raster::ExecuteProfile telemetry_profile_{};
};

}  // namespace micropixel::runtime

#endif
