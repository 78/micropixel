#include "platform/boards/esp32-s3-common/display_shadow.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "src/display/lv_display_private.h"

namespace micropixel::platform::esp32_s3_common {
namespace {

constexpr char kTag[] = "s3_display_shadow";
constexpr uint32_t kRgb565BytesPerPixel = 2U;
constexpr uint32_t kAllocationAlignment = 16U;

}  // namespace

esp_err_t DisplayShadow::Initialize(lv_display_t* display) {
    if (display == nullptr || display_ != nullptr || width_ == 0U || height_ == 0U || height_ > complete_rows_.size() ||
        width_ > UINT32_MAX / kRgb565BytesPerPixel || height_ > UINT32_MAX / (width_ * kRgb565BytesPerPixel)) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t bytes = width_ * height_ * kRgb565BytesPerPixel;
    pixels_ = static_cast<uint8_t*>(
        heap_caps_aligned_calloc(kAllocationAlignment, bytes, 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    adapter_flush_ = display->flush_cb;
    if (adapter_flush_ == nullptr || (owner_ != nullptr && owner_ != this)) {
        heap_caps_free(pixels_);
        pixels_ = nullptr;
        return ESP_ERR_INVALID_STATE;
    }
    display_ = display;
    owner_ = this;
    lv_display_set_flush_cb(display_, Flush);
    ESP_LOGI(kTag, "RGB565 displayed shadow allocated in PSRAM: bytes=%" PRIu32, bytes);
    return ESP_OK;
}

void DisplayShadow::Flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    DisplayShadow* owner = owner_;
    if (owner != nullptr && owner->display_ == display) {
        (void)owner->CopyArea(display, area, pixels);
    }
    if (owner != nullptr && owner->adapter_flush_ != nullptr) {
        owner->adapter_flush_(display, area, pixels);
    } else {
        lv_display_flush_ready(display);
    }
}

bool DisplayShadow::CopyArea(lv_display_t* display, const lv_area_t* area, const uint8_t* pixels) {
    if (pixels_ == nullptr || area == nullptr || pixels == nullptr || area->x1 < 0 || area->y1 < 0 ||
        area->x2 < area->x1 || area->y2 < area->y1 || area->x2 >= static_cast<int32_t>(width_) ||
        area->y2 >= static_cast<int32_t>(height_)) {
        return false;
    }
    const lv_draw_buf_t* active = lv_display_get_buf_active(display);
    if (active == nullptr || active->header.cf != LV_COLOR_FORMAT_RGB565) {
        return false;
    }
    const uint32_t area_width = static_cast<uint32_t>(area->x2 - area->x1 + 1);
    const uint32_t area_height = static_cast<uint32_t>(area->y2 - area->y1 + 1);
    const uint32_t copy_bytes = area_width * kRgb565BytesPerPixel;
    if (active->header.stride < copy_bytes) {
        return false;
    }
    uint8_t* destination =
        pixels_ + (static_cast<uint32_t>(area->y1) * width_ + static_cast<uint32_t>(area->x1)) * kRgb565BytesPerPixel;
    for (uint32_t row = 0U; row < area_height; ++row) {
        std::memcpy(destination + row * Stride(), pixels + row * active->header.stride, copy_bytes);
    }
    if (area->x1 == 0 && area->x2 == static_cast<int32_t>(width_) - 1) {
        for (int32_t y = area->y1; y <= area->y2; ++y) {
            complete_rows_[static_cast<size_t>(y)] = true;
        }
        if (!ready_) {
            ready_ = std::all_of(complete_rows_.begin(), complete_rows_.begin() + height_,
                                 [](bool complete) { return complete; });
        }
    }
    return true;
}

}  // namespace micropixel::platform::esp32_s3_common
