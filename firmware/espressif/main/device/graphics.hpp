#ifndef MICROPIXEL_DEVICE_GRAPHICS_HPP
#define MICROPIXEL_DEVICE_GRAPHICS_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::device {

struct BitmapView final {
    const uint8_t* data{};
    uint32_t size{};
    uint32_t width{};
    uint32_t height{};
    uint32_t stride{};
    uint32_t pixel_format{};
    uint32_t flags{};
};

using BitmapResolver = bool (*)(void* context, micropixel_bitmap_handle_t bitmap, BitmapView& view_out);

// Hardware-independent graphics contract. Concrete implementations live under
// platform/; board initialization is deliberately not part of this interface.
class GraphicsBackend {
   public:
    virtual ~GraphicsBackend() = default;

    [[nodiscard]] virtual bool Available() const = 0;
    [[nodiscard]] virtual int32_t GetInfo(micropixel_graphics_info_t& info) = 0;
    [[nodiscard]] virtual int32_t BeginFrame() = 0;
    [[nodiscard]] virtual int32_t Submit(const uint8_t* bytes, uint32_t length, BitmapResolver resolver,
                                         void* resolver_context) = 0;
    [[nodiscard]] virtual int32_t CommitFrame(BitmapResolver resolver, void* resolver_context) = 0;
    [[nodiscard]] virtual int32_t BeginBitmapUpdateFrame() = 0;
    [[nodiscard]] virtual int32_t UpdateBitmap(const BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width,
                                               uint32_t height, const uint8_t* pixels, uint32_t stride) = 0;
    [[nodiscard]] virtual int32_t CommitBitmapUpdateFrame() = 0;
    [[nodiscard]] virtual int32_t ShowLaunchBitmap(const BitmapView& bitmap) = 0;
    virtual void DismissLaunchBitmap() = 0;
    virtual void ReleaseGuestResources() = 0;
};

}  // namespace micropixel::device

#endif
