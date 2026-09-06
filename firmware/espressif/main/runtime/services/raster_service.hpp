#ifndef MICROPIXEL_RUNTIME_SERVICES_RASTER_SERVICE_HPP
#define MICROPIXEL_RUNTIME_SERVICES_RASTER_SERVICE_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"
#include "runtime/graphics/raster_kernels.hpp"
#include "runtime/services/direct_surface_service.hpp"
#include "runtime/services/service_result.hpp"

namespace micropixel::runtime {

// Graphics 1.6 raster kernels for one Session: owns the INDEX8 texture slots
// and the lit palette a Guest uploads, validates RASTER channel draw lists and
// runs them synchronously on the Guest task into a Host-owned Direct Surface
// buffer (in the panel's byte order; canonical colors are converted). Resource
// bytes are capped by `pool_bytes`; the Host passes kAbiPoolBytes, the most the
// ABI limits let one session upload, or 0 to withhold the kernels
// (CONFIG_MICROPIXEL_RASTER_KERNELS). Slots live in PSRAM unless
// CONFIG_MICROPIXEL_RASTER_POOL_INTERNAL_SRAM; everything is released at
// Shutdown().
class RasterService final {
   public:
    // MAX_TEXTURES full-size textures plus a palette with every light level.
    static constexpr uint32_t kAbiPoolBytes =
        MICROPIXEL_GRAPHICS_RASTER_MAX_TEXTURES * MICROPIXEL_GRAPHICS_RASTER_MAX_TEXTURE_SIZE *
            MICROPIXEL_GRAPHICS_RASTER_MAX_TEXTURE_SIZE +
        MICROPIXEL_GRAPHICS_RASTER_MAX_LIGHT_LEVELS * MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES * 2U;

    explicit RasterService(uint32_t pool_bytes);
    RasterService(const RasterService&) = delete;
    RasterService& operator=(const RasterService&) = delete;
    ~RasterService();

    void BindGuestMemory(const GuestMemoryAccess& access) { memory_ = access; }

    [[nodiscard]] bool available() const { return pool_bytes_ > 0U; }  // NOLINT(readability-identifier-naming)
    [[nodiscard]] uint32_t pool_bytes() const { return pool_bytes_; }  // NOLINT(readability-identifier-naming)

    [[nodiscard]] ServiceResult<void> UploadTexture(const micropixel_raster_texture_upload_request_t& request);
    [[nodiscard]] ServiceResult<void> UploadPalette(const micropixel_raster_palette_upload_request_t& request);
    // `surfaces` owns the target buffer and vetoes one the display still reads.
    [[nodiscard]] ServiceResult<void> Submit(const uint8_t* bytes, uint32_t length,
                                             const DirectSurfaceService& surfaces);

    void Shutdown();

   private:
    struct Slot final {
        uint8_t* pixels{};
        uint32_t bytes{};
    };

    [[nodiscard]] uint8_t* Allocate(uint32_t bytes);
    void Release(Slot& slot);
    // Validated Host pointer for a Guest range, or nullptr.
    [[nodiscard]] const uint8_t* ResolveGuest(uint32_t offset, uint32_t length) const;
    // Points `slot` at a fresh `bytes` allocation, freeing what it held; the
    // old resource survives when the new one cannot be allocated.
    [[nodiscard]] uint8_t* Replace(Slot& slot, uint32_t bytes);
    [[nodiscard]] raster::Resources ResourcesView() const;
    // Stores the palette in the byte order of the buffers it is copied into.
    void MatchPaletteByteOrder(bool byte_swapped);

    uint32_t pool_bytes_{};
    uint32_t used_bytes_{};
    GuestMemoryAccess memory_{};
    Slot slots_[MICROPIXEL_GRAPHICS_RASTER_MAX_TEXTURES]{};
    raster::Texture textures_[MICROPIXEL_GRAPHICS_RASTER_MAX_TEXTURES]{};
    Slot palette_slot_{};
    uint16_t palette_levels_{};
    bool palette_swapped_{};

    // Periodic kernel telemetry (CONFIG_MICROPIXEL_APP_SURFACE_TELEMETRY_LOG).
    static constexpr uint32_t kTelemetrySubmits = 240U;
    uint32_t telemetry_submits_{};
    uint32_t telemetry_records_{};
    uint32_t telemetry_bytes_{};
    uint64_t telemetry_execute_us_{};
};

}  // namespace micropixel::runtime

#endif
