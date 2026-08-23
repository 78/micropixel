#ifndef MICROPIXEL_RUNTIME_RESOURCES_BITMAP_STORE_HPP
#define MICROPIXEL_RUNTIME_RESOURCES_BITMAP_STORE_HPP

#include <cstddef>
#include <cstdint>

#include "abi/micropixel_abi.h"
#include "device/graphics.hpp"
#include "freertos/FreeRTOS.h"
#include "runtime/runtime_limits.hpp"

namespace micropixel::runtime {

// Owns Guest-visible Bitmap handles and the decoded-PSRAM quota. Asynchronous
// request scheduling remains the sole responsibility of ResourceService.
class BitmapStore final {
   public:
    BitmapStore() = default;
    BitmapStore(const BitmapStore&) = delete;
    BitmapStore& operator=(const BitmapStore&) = delete;
    ~BitmapStore();

    [[nodiscard]] micropixel_bitmap_handle_t Add(const device::BitmapView& view, bool owned,
                                                 bool mutable_pixels = false);
    [[nodiscard]] micropixel_bitmap_handle_t CreateOffscreenSurface(uint32_t width, uint32_t height,
                                                                    uint32_t pixel_format);
    [[nodiscard]] bool Resolve(micropixel_bitmap_handle_t bitmap, device::BitmapView& view_out) const;
    [[nodiscard]] bool ResolveMutable(micropixel_bitmap_handle_t bitmap, device::BitmapView& view_out) const;
    void Release(micropixel_bitmap_handle_t bitmap);
    void ReleaseAll();

   private:
    struct Slot final {
        micropixel_bitmap_handle_t handle{};
        device::BitmapView view{};
        bool owned{};
        bool mutable_pixels{};
    };

    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    Slot slots_[limits::kMaxBitmaps]{};
    uint32_t next_handle_{1U};
    size_t owned_bytes_{};
};

}  // namespace micropixel::runtime

#endif
