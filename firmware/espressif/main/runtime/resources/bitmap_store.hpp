#ifndef MICROPIXEL_RUNTIME_RESOURCES_BITMAP_STORE_HPP
#define MICROPIXEL_RUNTIME_RESOURCES_BITMAP_STORE_HPP

#include <cstddef>
#include <cstdint>

#include "abi/micropixel_abi.h"
#include "device/contracts/graphics.hpp"
#include "freertos/FreeRTOS.h"
#include "runtime/runtime_limits.hpp"

namespace micropixel::runtime {

// Owns Guest-visible Texture handles. Bitmap pixels allocate directly from
// PSRAM and are admitted until allocation fails. BitmapView is
// the internal pixel descriptor used by render implementations; the resource identity
// exposed across the ABI is always a Texture handle.
class BitmapStore final {
   public:
    BitmapStore();
    BitmapStore(const BitmapStore&) = delete;
    BitmapStore& operator=(const BitmapStore&) = delete;
    ~BitmapStore();

    [[nodiscard]] bool valid() const { return slots_ != nullptr; }  // NOLINT(readability-identifier-naming)
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
    [[nodiscard]] uint32_t HighWaterMark() const;

   private:
    enum SlotFlag : uint8_t {
        kStreaming = MICROPIXEL_TEXTURE_FLAG_STREAMING,
        kOwned = 1U << 5U,
        kMutablePixels = 1U << 6U,
        kGuestReference = 1U << 7U,
    };

    struct Slot final {
        const uint8_t* data{};
        uint32_t generation{};
        uint32_t scene_references{};
        uint16_t width{};
        uint16_t height{};
        uint16_t stride{};
        uint8_t pixel_format{};
        uint8_t flags{};
    };

    static constexpr uint32_t kHandleIndexBits = 8U;
    static constexpr uint32_t kHandleIndexMask = (1U << kHandleIndexBits) - 1U;
    static constexpr uint32_t kHandleGenerationMask = UINT32_MAX >> kHandleIndexBits;
    static constexpr uint8_t kPublicFlagMask = MICROPIXEL_TEXTURE_FLAG_STREAMING;

    [[nodiscard]] static micropixel_texture_handle_t MakeHandle(uint32_t index, uint32_t generation);
    [[nodiscard]] Slot* ResolveSlotLocked(micropixel_texture_handle_t bitmap);
    [[nodiscard]] const Slot* ResolveSlotLocked(micropixel_texture_handle_t bitmap) const;
    [[nodiscard]] static device::BitmapView View(const Slot& slot);
    static void ClearSlot(Slot& slot);

    static_assert(limits::kMaxBitmaps <= kHandleIndexMask);
    static_assert(sizeof(void*) != sizeof(uint32_t) || sizeof(Slot) == 20U);

    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    Slot* slots_{};
    uint32_t live_count_{};
    uint32_t high_water_mark_{};
};

}  // namespace micropixel::runtime

#endif
