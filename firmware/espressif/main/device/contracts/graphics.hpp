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

struct FontResourceView final {
    const uint8_t* data{};
    uint32_t size{};
};

using BitmapResolver = bool (*)(void* context, micropixel_texture_handle_t bitmap, BitmapView& view_out);
using FontValidator = bool (*)(void* context, micropixel_font_handle_t font);
using TextureRetainer = bool (*)(void* context, micropixel_texture_handle_t texture);
using TextureReleaser = void (*)(void* context, micropixel_texture_handle_t texture);

// Access to one Guest's Texture store. An implementation that retains pixel pointers
// past Submit() must retain every Texture referenced by the published scene and
// release the previous scene only after replacement succeeds.
struct TextureAccess final {
    void* context{};
    BitmapResolver resolve{};
    TextureRetainer retain{};
    TextureReleaser release{};
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
    [[nodiscard]] virtual int32_t ReleaseFont(micropixel_font_handle_t font) = 0;
    [[nodiscard]] virtual int32_t MeasureText(micropixel_font_handle_t font, const char* text, uint32_t text_length,
                                              micropixel_text_metrics_t& metrics_out) = 0;
    [[nodiscard]] virtual int32_t BeginBitmapUpdateFrame() = 0;
    [[nodiscard]] virtual int32_t UpdateBitmap(const BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width,
                                               uint32_t height, const uint8_t* pixels, uint32_t stride) = 0;
    [[nodiscard]] virtual int32_t CommitBitmapUpdateFrame() = 0;
    // One blocking hardware-assisted resize used by the Resource background
    // path. Source and destination have identical pixel formats.
    [[nodiscard]] virtual int32_t ScaleBitmap(const BitmapView& source, const BitmapView& destination) = 0;
    [[nodiscard]] virtual int32_t ShowLaunchBitmap(const BitmapView& bitmap) = 0;
    virtual void DismissLaunchBitmap() = 0;
    virtual void ReleaseGuestResources() = 0;
};

}  // namespace micropixel::device

#endif
