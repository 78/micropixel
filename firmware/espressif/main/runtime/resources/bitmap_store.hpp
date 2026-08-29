#ifndef MICROPIXEL_RUNTIME_RESOURCES_BITMAP_STORE_HPP
#define MICROPIXEL_RUNTIME_RESOURCES_BITMAP_STORE_HPP

#include <cstddef>
#include <cstdint>

#include "abi/micropixel_abi.h"
#include "device/contracts/graphics.hpp"
#include "freertos/FreeRTOS.h"
#include "runtime/runtime_limits.hpp"

namespace micropixel::runtime {

// Owns Guest-visible Texture handles. Bitmap pixels are admitted elastically
// while preserving the shared Host PSRAM safety floor. BitmapView is
// the internal pixel descriptor used by render implementations; the resource identity
// exposed across the ABI is always a Texture handle.
class BitmapStore final {
   public:
    BitmapStore() = default;
    BitmapStore(const BitmapStore&) = delete;
    BitmapStore& operator=(const BitmapStore&) = delete;
    ~BitmapStore();

    [[nodiscard]] micropixel_texture_handle_t Add(const device::BitmapView& view, bool owned,
                                                  bool mutable_pixels = false);
    [[nodiscard]] micropixel_texture_handle_t CreateOffscreenSurface(uint32_t width, uint32_t height,
                                                                     uint32_t pixel_format);
    [[nodiscard]] bool Resolve(micropixel_texture_handle_t bitmap, device::BitmapView& view_out) const;
    [[nodiscard]] bool ResolveMutable(micropixel_texture_handle_t bitmap, device::BitmapView& view_out) const;
    [[nodiscard]] bool RetainSceneReference(micropixel_texture_handle_t bitmap);
    void ReleaseSceneReference(micropixel_texture_handle_t bitmap);
    void Release(micropixel_texture_handle_t bitmap);
    void ReleaseAll();

   private:
    struct Slot final {
        micropixel_texture_handle_t handle{};
        device::BitmapView view{};
        uint32_t scene_references{};
        bool owned{};
        bool mutable_pixels{};
        bool guest_reference{};
    };

    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    Slot slots_[limits::kMaxBitmaps]{};
    uint32_t next_handle_{1U};
};

}  // namespace micropixel::runtime

#endif
