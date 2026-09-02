#ifndef MICROPIXEL_PLATFORM_LVGL_GUEST_GRAPHICS_OPERATIONS_HPP
#define MICROPIXEL_PLATFORM_LVGL_GUEST_GRAPHICS_OPERATIONS_HPP

#include "platform/adapters/graphics_adapter.hpp"
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
#include "platform/lvgl/display/ppa_srm_blitter.hpp"
#endif
#include "platform/lvgl/guest_graphics_engine.hpp"

namespace micropixel::platform::lvgl {

struct GuestGraphicsHooks final {
    void* context{};
    int32_t (*show_launch_bitmap)(void* context, const device::BitmapView& bitmap){};
    void (*dismiss_launch_bitmap)(void* context){};
};

// Process-lifetime binding owned by a Platform composition root. It keeps the
// hardware-neutral Graphics forwarding identical across boards while
// leaving launch-screen presentation in the selected LVGL board pipeline.
struct GuestGraphicsOperationsContext final {
    GuestGraphicsEngine* engine{};
    GuestGraphicsHooks hooks{};
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
    PpaSrmBlitter texture_scaler{};
#endif
};

[[nodiscard]] adapters::GraphicsOperations MakeGuestGraphicsOperations(GuestGraphicsOperationsContext& context);

}  // namespace micropixel::platform::lvgl

#endif
