#ifndef MICROPIXEL_DEVICE_GRAPHICS_HPP
#define MICROPIXEL_DEVICE_GRAPHICS_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::device {

// Graphics capacity policy shared by Runtime and Platform and reported to the
// Guest through micropixel_graphics_info_t. Scene nodes, containers and batch
// instances have no count cap: storage grows per submit up to the wire's
// uint16 ids and fails with RESOURCE_EXHAUSTED when PSRAM runs out. The values
// here bound one message, one text run and the Direct Surface ring.
namespace graphics_limits {
inline constexpr uint32_t kMaxTextBytes = 1024U;
inline constexpr uint32_t kMaxSceneBytes = 128U * 1024U;
inline constexpr uint32_t kMaxRasterBytes = 32768U;
inline constexpr uint32_t kMaxSurfaceBuffers = 3U;
}  // namespace graphics_limits

// Host-side BitmapView flag bits. They share the field with the public
// MICROPIXEL_TEXTURE_FLAG_* bits but never reach the Guest.
namespace bitmap_flags {
// RGB565 pixels are stored with the two bytes of every pixel swapped, the
// order of a MICROPIXEL_SURFACE_NATIVE_RGB565_BYTE_SWAPPED panel, so a raster
// kernel or DMA engine can copy them into a Host buffer verbatim.
inline constexpr uint32_t kRgb565ByteSwapped = 1U << 4U;
}  // namespace bitmap_flags

struct BitmapView final {
    const uint8_t* data{};
    uint32_t size{};
    uint32_t width{};
    uint32_t height{};
    uint32_t stride{};
    uint32_t pixel_format{};
    uint32_t flags{};
};

struct FontResourceView final {
    const uint8_t* data{};
    uint32_t size{};
};

using BitmapResolver = bool (*)(void* context, micropixel_texture_handle_t bitmap, BitmapView& view_out);
using FontValidator = bool (*)(void* context, micropixel_font_handle_t font_handle);
using TextureRetainer = bool (*)(void* context, micropixel_texture_handle_t texture_handle);
using TextureReleaser = void (*)(void* context, micropixel_texture_handle_t texture_handle);

// Access to one Guest's Texture store. An implementation that retains pixel pointers
// past Submit() must retain every Texture referenced by the published scene and
// release the previous scene only after replacement succeeds.
struct TextureAccess final {
    void* context{};
    BitmapResolver resolve{};
    TextureRetainer retain{};
    TextureReleaser release{};
};

// Graphics 1.5 Direct Surface: one set of full-frame RGB565 buffers (owned by
// the Host service or, for PINNED_MEMORY Bundles, living in Guest linear
// memory) that the Host scans out or composites in place. The device only
// reads a buffer between Present() and the matching release. `flags` is
// reserved and must be 0.
struct DirectSurfaceConfig final {
    uint32_t width{};
    uint32_t height{};
    uint32_t pixel_format{};
    uint32_t buffer_count{};
    uint32_t flags{};
};

struct DirectSurfaceInfo final {
    uint32_t native_pixel_format{};
    uint32_t native_flags{};
    uint16_t max_full_frame_fps{};
};

struct DirectSurfacePresentation final {
    // Host-native pointer into validated Guest memory; stays valid until the
    // release callback names buffer_index. The Host owns the contents while the
    // buffer is in flight and may scribble on it (e.g. blend a system overlay
    // in place before scanout) as long as the Guest's pixels are restored
    // before the release callback fires.
    uint8_t* pixels{};
    uint32_t length{};
    uint32_t pitch{};
    uint32_t source_width{};
    uint32_t source_height{};
    uint32_t flags{};
    uint8_t buffer_index{};
    // Pixels hold RGB565 with the two bytes of every pixel swapped (the order
    // of a MICROPIXEL_SURFACE_NATIVE_RGB565_BYTE_SWAPPED panel). Presented
    // buffers are always in panel order, so this mirrors the panel's flag.
    bool byte_swapped{};
};

// RGB565 rows the Host draws into on behalf of a raster record (TEXT, or an
// opaque IMAGE block copy): a Host-owned Direct Surface buffer that is not in
// flight. `byte_swapped` mirrors DirectSurfacePresentation::byte_swapped.
struct TextTarget final {
    uint8_t* pixels{};
    uint32_t width{};
    uint32_t height{};
    uint32_t pitch{};
    bool byte_swapped{};
};
using PixelTarget = TextTarget;

// One unscaled, opaque RGB565 rectangle copy from a texture into a PixelTarget,
// already clipped by the caller: the destination rectangle lies inside the
// target and the source rectangle inside the texture. Source pixels are in the
// target's byte order (bitmap_flags::kRgb565ByteSwapped agrees with
// PixelTarget::byte_swapped), so the device moves bytes without conversion.
struct OpaqueCopyBlock final {
    const uint8_t* source_pixels{};
    uint32_t source_stride{};
    uint32_t source_picture_width{};
    uint32_t source_picture_height{};
    uint32_t source_x{};
    uint32_t source_y{};
    uint32_t destination_x{};
    uint32_t destination_y{};
    uint32_t width{};
    uint32_t height{};
};

// Called from a Host task (never the Guest task) once the Host no longer reads
// buffer_index. Implementations must be able to invoke it while Present() or
// SuspendDirectSurface() is executing on another task.
using DirectSurfaceReleaser = void (*)(void* context, uint8_t buffer_index, uint64_t timestamp_us);

struct DirectSurfaceReleaseSink final {
    void* context{};
    DirectSurfaceReleaser release{};
};

// Hardware-independent graphics contract. Concrete implementations live under
// platform/; board initialization is deliberately not part of this interface.
class Graphics {
   public:
    virtual ~Graphics() = default;

    [[nodiscard]] virtual bool Available() const = 0;
    [[nodiscard]] virtual int32_t GetInfo(micropixel_graphics_info_t& info) = 0;
    [[nodiscard]] virtual int32_t Submit(const uint8_t* bytes, uint32_t length, const TextureAccess& textures) = 0;
    [[nodiscard]] virtual int32_t LoadFont(const FontResourceView& resource, micropixel_font_info_t& info_out) = 0;
    [[nodiscard]] virtual int32_t ReleaseFont(micropixel_font_handle_t font_handle) = 0;
    [[nodiscard]] virtual int32_t MeasureText(micropixel_font_handle_t font_handle, const char* text,
                                              uint32_t text_length, micropixel_text_metrics_t& metrics_out) = 0;
    // Paints `text` with `font_handle` (the same handles MeasureText accepts)
    // in `rgb888`, top-left at (x, y), clipped to the target; glyph coverage is
    // blended over the existing pixels. Synchronous on the calling task.
    // Copies every block in order (later blocks land on top of earlier ones)
    // and returns only once the pixels are in memory and visible to the CPU.
    // UNSUPPORTED when the device has no copy engine; any other failure leaves
    // the caller to draw the same blocks itself, so partial writes are
    // harmless as long as the caller then rewrites every block.
    [[nodiscard]] virtual int32_t CopyOpaqueBlocks(const PixelTarget& target, const OpaqueCopyBlock* blocks,
                                                   uint32_t count) = 0;
    [[nodiscard]] virtual int32_t DrawText(const TextTarget& target, int32_t x, int32_t y, uint32_t rgb888,
                                           micropixel_font_handle_t font_handle, const char* text,
                                           uint32_t text_length) = 0;
    // One blocking hardware-assisted resize used by the Resource background
    // path. Source and destination have identical pixel formats.
    [[nodiscard]] virtual int32_t ScaleBitmap(const BitmapView& source, const BitmapView& destination) = 0;
    [[nodiscard]] virtual int32_t ShowLaunchBitmap(const BitmapView& bitmap) = 0;
    virtual void DismissLaunchBitmap() = 0;
    virtual void ReleaseGuestResources() = 0;

    // Direct Surface. At most one per Guest; Create fails with
    // RESOURCE_EXHAUSTED while one exists. Present queues the buffer and
    // returns immediately. SuspendDirectSurface stops scanning out and returns
    // every in-flight buffer through the sink before it returns; the surface
    // stays created, and until ResumeDirectSurface every further Present is
    // released unshown (the Guest may still present on its way to the suspend
    // safe point). DestroyDirectSurface returns the buffers the same way and
    // then invalidates the surface. ReleaseGuestResources implies
    // DestroyDirectSurface.
    //
    // Suspend/Resume are also called when no Direct Surface exists: a device
    // may scan the composited Scene frames out through the same exclusive
    // path, and the Host UI must get the panel back on pause just the same.
    [[nodiscard]] virtual int32_t CreateDirectSurface(const DirectSurfaceConfig& config,
                                                      const DirectSurfaceReleaseSink& sink,
                                                      DirectSurfaceInfo& info_out) = 0;
    [[nodiscard]] virtual int32_t PresentDirectSurface(const DirectSurfacePresentation& presentation) = 0;
    virtual void SuspendDirectSurface() = 0;
    virtual void ResumeDirectSurface() = 0;
    [[nodiscard]] virtual int32_t DestroyDirectSurface() = 0;
};

}  // namespace micropixel::device

#endif
