#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "abi/micropixel_abi.h"
#include "lvgl.h"
#include "platform/metalio-claw4/fonts/font_cbin_loader.hpp"

namespace micropixel::platform::metalio_claw4 {

enum class SystemFontRole : uint8_t {
    kSmall,
    kMedium,
    kLarge,
    kTitle,
    kCount,
};

struct SystemFontSet final {
    std::array<const lv_font_t*, static_cast<size_t>(SystemFontRole::kCount)> fonts{};
};

[[nodiscard]] const lv_font_t* BuiltinLatinFont(SystemFontRole role);

// Platform-owned indirection between stable wire handles and the active LVGL
// fonts. Candidate sets are fully validated before replacing the four active
// pointers, so callers never observe a partially activated language set.
class FontRegistry final {
   public:
    static constexpr uint32_t kMaxDynamicFonts = 8U;

    FontRegistry();
    FontRegistry(const FontRegistry&) = delete;
    FontRegistry& operator=(const FontRegistry&) = delete;
    ~FontRegistry();

    [[nodiscard]] const lv_font_t* Resolve(SystemFontRole role) const;
    [[nodiscard]] const lv_font_t* ResolveSystemHandle(uint16_t handle) const;
    [[nodiscard]] const lv_font_t* ResolveGuestHandle(micropixel_font_handle_t handle) const;
    [[nodiscard]] const lv_font_t* ResolveRetainedHandle(micropixel_font_handle_t handle) const;
    [[nodiscard]] int32_t LoadFont(std::span<const uint8_t> package, micropixel_font_info_t& info_out);
    [[nodiscard]] int32_t ReleaseFont(micropixel_font_handle_t handle);
    [[nodiscard]] bool RetainSceneFont(micropixel_font_handle_t handle);
    void ReleaseSceneFont(micropixel_font_handle_t handle);
    void ReleaseGuestFonts();
    [[nodiscard]] bool Activate(const SystemFontSet& candidate);
    void ResetToBuiltin();

    [[nodiscard]] uint32_t generation() const {  // NOLINT(readability-identifier-naming)
        return generation_;
    }

   private:
    struct DynamicFontSlot final {
        std::unique_ptr<LoadedCbinFont> loaded{};
        uint16_t generation{};
        uint16_t scene_references{};
        bool guest_owned{};
    };

    [[nodiscard]] static bool Valid(const SystemFontSet& candidate);
    [[nodiscard]] DynamicFontSlot* FindDynamic(micropixel_font_handle_t handle);
    [[nodiscard]] const DynamicFontSlot* FindDynamic(micropixel_font_handle_t handle) const;
    void Reclaim(DynamicFontSlot& slot);

    SystemFontSet active_{};
    std::array<DynamicFontSlot, kMaxDynamicFonts> dynamic_{};
    uint32_t generation_{1U};
};

}  // namespace micropixel::platform::metalio_claw4
