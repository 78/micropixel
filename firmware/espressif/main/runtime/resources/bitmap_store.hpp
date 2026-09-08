#ifndef MICROPIXEL_RUNTIME_RESOURCES_BITMAP_STORE_HPP
#define MICROPIXEL_RUNTIME_RESOURCES_BITMAP_STORE_HPP

#include <cstddef>
#include <cstdint>

#include "abi/micropixel_abi.h"
#include "device/contracts/graphics.hpp"
#include "freertos/FreeRTOS.h"
#include "runtime/runtime_limits.hpp"
#include "runtime/services/service_result.hpp"

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
    [[nodiscard]] micropixel_texture_handle_t Add(const device::BitmapView& view, bool owned);
    [[nodiscard]] ServiceResult<micropixel_texture_handle_t> CreateDynamic(uint32_t width, uint32_t height,
                                                                           uint32_t format, const uint8_t* pixels,
                                                                           uint32_t length, uint32_t pitch);
    [[nodiscard]] ServiceResult<micropixel_texture_handle_t> UpdateDynamic(micropixel_texture_handle_t source,
                                                                           uint32_t x, uint32_t y, uint32_t width,
                                                                           uint32_t height, const uint8_t* pixels,
                                                                           uint32_t length, uint32_t pitch);
    [[nodiscard]] bool Resolve(micropixel_texture_handle_t bitmap, device::BitmapView& view_out) const;
    [[nodiscard]] bool RetainSceneReference(micropixel_texture_handle_t bitmap);
    void ReleaseSceneReference(micropixel_texture_handle_t bitmap);
    void Release(micropixel_texture_handle_t bitmap);
    void ReleaseAll();
    [[nodiscard]] uint32_t HighWaterMark() const;

   private:
    // Bits above kPublicFlagMask are Host bookkeeping and never reach the Guest.
    enum SlotFlag : uint8_t {
        kOwned = 1U << 5U,
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
    static constexpr uint8_t kPublicFlagMask = MICROPIXEL_TEXTURE_FLAG_DYNAMIC;

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
