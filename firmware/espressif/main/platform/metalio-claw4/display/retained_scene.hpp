#pragma once

#include <cstdint>

#include "device/graphics.hpp"
#include "sdkconfig.h"
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
#include "platform/metalio-claw4/display/retained_surface.hpp"
#endif

namespace micropixel::platform::metalio_claw4 {

struct RetainedFrameResult final {
    int32_t status{};
    bool visual_changed{};
    bool surface_active{};
};

class RetainedScene final {
   public:
    RetainedScene(int32_t logical_width, int32_t logical_height)
        : logical_width_(logical_width), logical_height_(logical_height) {}

#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
    void BindSurface(lv_display_t* display, esp_lcd_panel_handle_t panel);
#endif
    [[nodiscard]] bool Initialize();
    void ForgetObjects();
    void Release();
    [[nodiscard]] RetainedFrameResult Execute(const uint8_t* bytes, uint32_t length, lv_obj_t* frame,
                                              device::BitmapResolver resolver, void* resolver_context);
    [[nodiscard]] bool InvalidateBitmap(const uint8_t* data, uint32_t x, uint32_t y, uint32_t width, uint32_t height);

    [[nodiscard]] bool SurfaceActive() const;

   private:
    struct RetainedObject final {
        lv_obj_t* object{};
        lv_obj_t* parent{};
        uint16_t opcode{};
        micropixel_bitmap_handle_t bitmap{};
        lv_image_dsc_t image{};
        char text[MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES + 1U]{};
        uint16_t text_length{};
        int32_t x{};
        int32_t y{};
        int32_t source_x{};
        int32_t source_y{};
        int32_t width{};
        int32_t height{};
        uint32_t rgb888{};
        uint8_t opacity{};
        const lv_font_t* font{};
        bool state_valid{};
        bool visible{};
    };

    void DiscardObject(RetainedObject& slot);
    void DiscardAllObjects();
    [[nodiscard]] bool TopologyChanged(const uint8_t* bytes, uint32_t length,
                                       const micropixel_graphics_command_header_t& header) const;
    [[nodiscard]] RetainedObject& PrepareObject(uint32_t index, uint16_t opcode, lv_obj_t* frame, int32_t frame_width,
                                                bool& changed, bool& order_dirty);
    void SetObjectVisible(RetainedObject& slot, bool visible, bool& changed);
    [[nodiscard]] bool IsFillPlaceholder(const micropixel_graphics_fill_rect_command_t& command) const;
    [[nodiscard]] bool IsTextPlaceholder(const micropixel_graphics_draw_text_command_t& command,
                                         const char* text) const;

    int32_t logical_width_{};
    int32_t logical_height_{};
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
    RetainedSurface surface_{};
#endif
    RetainedObject* objects_{};
    uint32_t object_count_{};
    uint32_t last_used_{};
    uint32_t background_rgb888_{};
    bool background_valid_{};
};

}  // namespace micropixel::platform::metalio_claw4
