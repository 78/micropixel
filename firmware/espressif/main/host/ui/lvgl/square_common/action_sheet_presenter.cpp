// SPDX-License-Identifier: Apache-2.0
#include "host/ui/lvgl/square_common/action_sheet_presenter.hpp"

#include <cinttypes>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "host/ui/lvgl/square_common/host_ui_theme.hpp"
#include "host/ui/lvgl/square_common/status_layer_transition.hpp"
#include "platform/lvgl/display/scanout_arbiter.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"
#include "src/core/lv_refr_private.h"

namespace micropixel::host_ui::lvgl::square_common {

void ActionSheetPresenter::AnimateSoftwareLocked(const SystemPageLayout& layout, lv_obj_t* sheet) {
    // Translate independently of bottom alignment and content-driven height.
    // LVGL removes animations targeting the sheet when the object is deleted.
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, sheet);
    lv_anim_set_values(&animation, layout.height, 0);
    lv_anim_set_duration(&animation, 150U);
    lv_anim_set_start_cb(&animation, [](lv_anim_t* active) {
        auto* sheet = static_cast<lv_obj_t*>(active->var);
        // Children are populated after CreateActionSheet returns. Resolve the
        // final height on the first animation tick, then start at the bottom
        // edge rather than spending most of the animation below the screen.
        lv_obj_update_layout(sheet);
        lv_anim_set_values(active, lv_obj_get_height(sheet) - lv_obj_get_style_y(sheet, LV_PART_MAIN), 0);
        lv_anim_set_delay(active, 0);
    });
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&animation, [](void* object, int32_t offset) {
        auto* sheet = static_cast<lv_obj_t*>(object);
        lv_obj_set_style_translate_y(sheet, offset, 0);
        // Invalidation alone waits for the slow static-scene refresh timer.
        // Publish every animation step, including the final resting position.
        platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(sheet));
    });
    lv_anim_start(&animation);
}

void ActionSheetPresenter::RequestLocked(const SystemPageLayout& layout, lv_obj_t* sheet, SystemUiActionSink sink,
                                         void* context) {
    CancelLocked();
    if (sink == nullptr) {
        AnimateSoftwareLocked(layout, sheet);
        return;
    }
    layout_ = &layout;
    sheet_ = sheet;
    lv_obj_add_event_cb(sheet_, SheetDeleted, LV_EVENT_DELETE, this);
    // The transparent overlay still intercepts taps while the Host is waking.
    // Hide only its sheet so no final-position flash precedes the snapshot.
    lv_obj_set_style_bg_opa(lv_obj_get_parent(sheet_), LV_OPA_TRANSP, 0);
    lv_obj_add_flag(sheet_, LV_OBJ_FLAG_HIDDEN);
    sink(context, SystemUiAction{.type = SystemUiActionType::kPresentActionSheet});
}

void ActionSheetPresenter::SheetDeleted(lv_event_t* event) {
    auto* presenter = static_cast<ActionSheetPresenter*>(lv_event_get_user_data(event));
    if (presenter->sheet_ == lv_event_get_target_obj(event)) {
        presenter->sheet_ = nullptr;
        presenter->layout_ = nullptr;
    }
}

void ActionSheetPresenter::CancelLocked() {
    if (sheet_ == nullptr) return;
    lv_obj_remove_event_cb_with_user_data(sheet_, SheetDeleted, this);
    lv_obj_set_style_bg_opa(lv_obj_get_parent(sheet_), LV_OPA_80, 0);
    lv_obj_remove_flag(sheet_, LV_OBJ_FLAG_HIDDEN);
    sheet_ = nullptr;
    layout_ = nullptr;
}

void ActionSheetPresenter::Present() {
    const platform::lvgl::SystemScanoutScope scanout_scope;
    if (esp_lv_adapter_lock(-1) != ESP_OK) return;
    if (sheet_ == nullptr) {
        esp_lv_adapter_unlock();
        return;
    }
    const int64_t started_us = esp_timer_get_time();
    lv_obj_t* sheet = sheet_;
    const SystemPageLayout& layout = *layout_;
    lv_display_t* display = lv_obj_get_display(sheet);
    // Resolve page rebuilds/scroll positions before retaining their background.
    lv_refr_now(display);
    const bool started = transition_.BeginStatusLayerTransition(true, theme::kModalScrim, LV_OPA_80, 0U);
    CancelLocked();
    lv_obj_update_layout(sheet);
    bool finished = false;
    if (started) {
        if (transition_.AnimateStatusLayerLocked(sheet, lv_obj_get_y(sheet),
                                                 lv_display_get_vertical_resolution(display), true, 100U, 0U)) {
            (void)lv_inv_area(display, nullptr);
            finished = transition_.FinishStatusLayerTransition(false);
        }
        if (!finished) transition_.CancelStatusLayerTransition();
    }
    if (!finished) {
        lv_obj_invalidate(lv_obj_get_parent(sheet));
        AnimateSoftwareLocked(layout, sheet);
    }
    esp_lv_adapter_unlock();
    ESP_LOGI("action_sheet", "transition: mode=%s prepare-and-present=%" PRId64 " us", finished ? "PPA" : "LVGL",
             esp_timer_get_time() - started_us);
}

}  // namespace micropixel::host_ui::lvgl::square_common
