#pragma once

#include <cstdint>

#include "abi/micropixel_abi.h"
#include "driver/ppa.h"
#include "esp_async_color_convert.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

namespace micropixel::platform::metalio_claw4 {

// Direct panel-compositor state for one translated Guest surface.
class RetainedSurface final {
   public:
    void Bind(lv_display_t* display, esp_lcd_panel_handle_t panel, uint32_t display_width, uint32_t display_height);
    [[nodiscard]] bool Configure(lv_obj_t* root, const micropixel_graphics_begin_surface_command_t& command,
                                 bool background_valid, uint32_t background_rgb888);
    void SetBackground(uint32_t rgb888);
    [[nodiscard]] bool Update(const micropixel_graphics_begin_surface_command_t* request);
    void Release();

    [[nodiscard]] lv_obj_t* Frame() const { return frame_; }
    [[nodiscard]] bool Configured() const { return configured_; }
    [[nodiscard]] bool Active() const { return active_; }

   private:
    static constexpr uint32_t kFramebufferCount = 2U;

    struct FrameState final {
        uint8_t* buffer{};
        int32_t translate_x{};
        int32_t translate_y{};
        uint32_t generation{};
    };

    struct SurfaceRect final {
        int32_t x{};
        int32_t y{};
        int32_t width{};
        int32_t height{};
    };

    [[nodiscard]] esp_err_t CopyRgb888(const uint8_t* source, uint32_t source_width, uint32_t source_height,
                                       uint32_t source_x, uint32_t source_y, uint8_t* destination,
                                       uint32_t destination_width, uint32_t destination_height, uint32_t destination_x,
                                       uint32_t destination_y, uint32_t width, uint32_t height);
    [[nodiscard]] esp_err_t RestoreRect(uint8_t* destination, const SurfaceRect& rect);
    [[nodiscard]] esp_err_t RestoreExposed(uint8_t* destination, int32_t old_translate_x, int32_t old_translate_y,
                                           int32_t new_translate_x, int32_t new_translate_y);
    [[nodiscard]] FrameState* FindFrameState(uint8_t* frame_buffer);
    [[nodiscard]] bool AllFramebuffersRestored() const;
    [[nodiscard]] bool ComposeFrame(int32_t translate_x, int32_t translate_y, uint8_t* acquired_frame_buffer,
                                    uint32_t& acquire_us, uint32_t& restore_us, uint32_t& surface_copy_us,
                                    uint32_t& flush_us);

    lv_display_t* display_{};
    esp_lcd_panel_handle_t panel_{};
    lv_obj_t* frame_{};
    uint8_t* pixels_{};
    async_color_convert_handle_t dma2d_client_{};
    ppa_client_handle_t fill_client_{};
    FrameState frame_states_[kFramebufferCount]{};
    uint32_t display_width_{};
    uint32_t display_height_{};
    uint32_t surface_bytes_{};
    uint32_t background_rgb888_{};
    int32_t x_{};
    int32_t y_{};
    int32_t width_{};
    int32_t height_{};
    int32_t translate_x_{};
    int32_t translate_y_{};
    uint32_t capture_count_{};
    uint32_t translate_count_{};
    uint32_t generation_{};
    bool configured_{};
    bool active_{};
};

}  // namespace micropixel::platform::metalio_claw4
