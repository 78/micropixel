#pragma once

#include <array>
#include <cstdint>

#include "esp_err.h"
#include "lvgl.h"

namespace micropixel::platform::esp32_s3_common {

// Maintains a canonical little-endian RGB565 copy of the pixels submitted by
// LVGL. The panel adapter may byte-swap its partial buffers after this wrapper
// returns, so screenshots must read this pre-transport PSRAM shadow instead.
class DisplayShadow final {
   public:
    DisplayShadow(uint32_t width, uint32_t height) : width_(width), height_(height) {}

    [[nodiscard]] esp_err_t Initialize(lv_display_t* display);
    [[nodiscard]] const uint8_t* Pixels() const { return pixels_; }
    [[nodiscard]] uint32_t Stride() const { return width_ * 2U; }
    [[nodiscard]] const bool* Ready() const { return &ready_; }

   private:
    static void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels);
    [[nodiscard]] bool CopyArea(lv_display_t* display, const lv_area_t* area, const uint8_t* pixels);

    static inline DisplayShadow* owner_{};
    lv_display_t* display_{};
    lv_display_flush_cb_t adapter_flush_{};
    uint8_t* pixels_{};
    std::array<bool, 240U> complete_rows_{};
    uint32_t width_{};
    uint32_t height_{};
    bool ready_{};
};

}  // namespace micropixel::platform::esp32_s3_common
