#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_GRAPHICS_ADAPTER_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_GRAPHICS_ADAPTER_HPP

#include "device/graphics.hpp"

namespace micropixel::platform::metalio_claw4 {

struct GraphicsOperations final {
    void* context{};
    bool (*available)(void*){};
    int32_t (*get_info)(void*, micropixel_graphics_info_t&){};
    int32_t (*begin_frame)(void*){};
    int32_t (*submit)(void*, const uint8_t*, uint32_t, device::BitmapResolver, void*){};
    int32_t (*commit_frame)(void*, device::BitmapResolver, void*){};
    int32_t (*begin_bitmap_update_frame)(void*){};
    int32_t (*update_bitmap)(void*, const device::BitmapView&, uint32_t, uint32_t, uint32_t, uint32_t, const uint8_t*,
                             uint32_t){};
    int32_t (*commit_bitmap_update_frame)(void*){};
    int32_t (*show_launch_bitmap)(void*, const device::BitmapView&){};
    void (*dismiss_launch_bitmap)(void*){};
    void (*release_guest_resources)(void*){};
};

class GraphicsAdapter final : public device::GraphicsBackend {
   public:
    explicit GraphicsAdapter(GraphicsOperations operations) : operations_(operations) {}

    [[nodiscard]] bool Available() const override;
    [[nodiscard]] int32_t GetInfo(micropixel_graphics_info_t& info) override;
    [[nodiscard]] int32_t BeginFrame() override;
    [[nodiscard]] int32_t Submit(const uint8_t* bytes, uint32_t length, device::BitmapResolver resolver,
                                 void* resolver_context) override;
    [[nodiscard]] int32_t CommitFrame(device::BitmapResolver resolver, void* resolver_context) override;
    [[nodiscard]] int32_t BeginBitmapUpdateFrame() override;
    [[nodiscard]] int32_t UpdateBitmap(const device::BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width,
                                       uint32_t height, const uint8_t* pixels, uint32_t stride) override;
    [[nodiscard]] int32_t CommitBitmapUpdateFrame() override;
    [[nodiscard]] int32_t ShowLaunchBitmap(const device::BitmapView& bitmap) override;
    void DismissLaunchBitmap() override;
    void ReleaseGuestResources() override;

   private:
    GraphicsOperations operations_;
};

}  // namespace micropixel::platform::metalio_claw4

#endif
