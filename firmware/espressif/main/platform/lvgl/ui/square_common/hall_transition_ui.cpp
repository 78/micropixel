#include "platform/lvgl/ui/square_common/hall_transition_ui.hpp"

#include <algorithm>

#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::platform::lvgl::square_common {
namespace {

bool ValidFrame(const HallTransitionFrame& frame) {
    return frame.data != nullptr && frame.width > 0U && frame.height > 0U && frame.stride >= frame.width * 3U &&
           frame.size >= frame.stride * frame.height;
}

}  // namespace

void HallTransitionUi::Initialize(lv_display_t* display, uint32_t width, uint32_t height) {
    display_ = display;
    width_ = width;
    height_ = height;
}

bool HallTransitionUi::PrepareLocked(HallTransitionFrame intermediate, HallTransitionFrame cover,
                                     const HallTransitionRect& card, HallTransitionDirection direction) {
    FinishLocked();
    if (display_ == nullptr || !ValidFrame(intermediate) || !ValidFrame(cover) || width_ == 0U || height_ == 0U ||
        intermediate.width > width_ || intermediate.height > height_ ||
        cover.width != static_cast<uint32_t>(card.width) || cover.height != static_cast<uint32_t>(card.height) ||
        card.x < 0 || card.y < 0 || card.width <= 0 || card.height <= 0 ||
        card.x + card.width > static_cast<int32_t>(width_) || card.y + card.height > static_cast<int32_t>(height_)) {
        return false;
    }

    frames_[0] = intermediate;
    frames_[1] = cover;
    for (size_t index = 0U; index < descriptors_.size(); ++index) {
        descriptors_[index] = {};
        descriptors_[index].header.magic = LV_IMAGE_HEADER_MAGIC;
        descriptors_[index].header.cf = LV_COLOR_FORMAT_RGB888;
        descriptors_[index].header.w = frames_[index].width;
        descriptors_[index].header.h = frames_[index].height;
        descriptors_[index].header.stride = frames_[index].stride;
        descriptors_[index].data_size = frames_[index].size;
        descriptors_[index].data = frames_[index].data;
    }
    card_ = card;
    overlay_ = lv_image_create(lv_screen_active());
    if (overlay_ == nullptr) {
        descriptors_ = {};
        frames_ = {};
        return false;
    }
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);
    ShowFrameLocked(direction == HallTransitionDirection::kToHall ? 0U : 1U);
    return true;
}

void HallTransitionUi::ShowFrameLocked(size_t index) {
    if (overlay_ == nullptr || index >= frames_.size()) {
        return;
    }
    lv_image_set_src(overlay_, &descriptors_[index]);
    lv_obj_set_size(overlay_, static_cast<int32_t>(frames_[index].width), static_cast<int32_t>(frames_[index].height));
    if (index == 0U) {
        lv_obj_set_pos(overlay_, (static_cast<int32_t>(width_) - static_cast<int32_t>(frames_[index].width)) / 2,
                       (static_cast<int32_t>(height_) - static_cast<int32_t>(frames_[index].height)) / 2);
    } else {
        lv_obj_set_pos(overlay_, card_.x, card_.y);
    }
    lv_obj_move_foreground(overlay_);
}

bool HallTransitionUi::Animate(HallTransitionDirection direction, HallTransitionTimeline timeline) {
    if (overlay_ == nullptr || display_ == nullptr) {
        return false;
    }
    const TickType_t stage_delay = pdMS_TO_TICKS(std::max<uint32_t>(1U, timeline.duration_ms / 2U));
    vTaskDelay(stage_delay);
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return false;
    }
    ShowFrameLocked(direction == HallTransitionDirection::kToHall ? 1U : 0U);
    RequestDisplayRefresh(display_);
    esp_lv_adapter_unlock();
    vTaskDelay(stage_delay);
    return true;
}

void HallTransitionUi::FinishLocked() {
    if (overlay_ != nullptr) {
        lv_obj_delete(overlay_);
        overlay_ = nullptr;
    }
    descriptors_ = {};
    frames_ = {};
    card_ = {};
}

}  // namespace micropixel::platform::lvgl::square_common
