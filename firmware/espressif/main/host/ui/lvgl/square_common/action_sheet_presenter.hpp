// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host/ui/lvgl/square_common/system_page_layout.hpp"
#include "host/ui/system_ui.hpp"

namespace micropixel::host_ui::lvgl::square_common {

class StatusLayerTransition;

// One pending sheet per display. Requests and object deletion run under the
// LVGL lock; Present runs on the Shell task and acquires scanout before LVGL.
class ActionSheetPresenter final {
   public:
    explicit ActionSheetPresenter(StatusLayerTransition& transition) : transition_(transition) {}
    ActionSheetPresenter(const ActionSheetPresenter&) = delete;
    ActionSheetPresenter& operator=(const ActionSheetPresenter&) = delete;

    void RequestLocked(const SystemPageLayout& layout, lv_obj_t* sheet, SystemUiActionSink sink, void* context);
    void CancelLocked();
    void Present();
    static void AnimateSoftwareLocked(const SystemPageLayout& layout, lv_obj_t* sheet);

   private:
    static void SheetDeleted(lv_event_t* event);
    StatusLayerTransition& transition_;
    const SystemPageLayout* layout_{};
    lv_obj_t* sheet_{};
};

}  // namespace micropixel::host_ui::lvgl::square_common
