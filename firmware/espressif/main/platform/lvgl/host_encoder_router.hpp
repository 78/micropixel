#ifndef MICROPIXEL_PLATFORM_LVGL_HOST_ENCODER_ROUTER_HPP
#define MICROPIXEL_PLATFORM_LVGL_HOST_ENCODER_ROUTER_HPP

#include <atomic>
#include <cstdint>

#include "esp_err.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::platform::lvgl {

// Publishes a rotary control such as the SenseCAP Watcher's knob as an LVGL
// encoder, so rotation moves the focus between the focusable widgets of the
// active screen and a press activates the focused one.
//
// No screen has to know about the knob. LVGL adds every object whose class is
// "group default" - buttons, switches, text areas and friends - to the default
// group as it is constructed, so installing the group before the UI builds its
// screens makes all of them, including screens built later, participate. The
// theme's own focus style marks the focused widget, which is why this router
// carries no styling of its own.
//
// That single group holds the widgets of every screen, so LVGL's own encoder
// navigation walks them across screen boundaries. On the Hall it moved the
// focus onto widgets that are not on the screen at all, which is what "turning
// the wheel does not select the Settings button" looks like: the outline went
// somewhere invisible, and one click could cross several widgets. This router
// therefore moves the focus itself, through the navigable widgets of the active
// screen only, and hands LVGL no encoder diff to act on.
//
// This is opt-in: a board whose input cannot rotate never calls InitializeLocked
// and nothing in the shared UI changes for it.
class HostEncoderRouter final {
   public:
    // Must be called while the caller owns the LVGL lock, and before the UI
    // builds its screens. The router must outlive the input device it creates.
    [[nodiscard]] esp_err_t InitializeLocked(lv_display_t* display) {
        if (display == nullptr) {
            return ESP_ERR_INVALID_ARG;
        }
        if (indev_ != nullptr || group_ != nullptr) {
            return ESP_ERR_INVALID_STATE;
        }
        group_ = lv_group_create();
        if (group_ == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        // Objects join the default group as they are constructed.
        lv_group_set_default(group_);
        indev_ = lv_indev_create();
        if (indev_ == nullptr) {
            lv_group_set_default(nullptr);
            lv_group_delete(group_);
            group_ = nullptr;
            return ESP_ERR_NO_MEM;
        }
        display_ = display;
        lv_indev_set_type(indev_, LV_INDEV_TYPE_ENCODER);
        lv_indev_set_mode(indev_, LV_INDEV_MODE_EVENT);
        lv_indev_set_read_cb(indev_, Read);
        lv_indev_set_user_data(indev_, this);
        lv_indev_set_display(indev_, display_);
        lv_indev_set_group(indev_, group_);
        return ESP_OK;
    }

    [[nodiscard]] bool Available() const { return indev_ != nullptr; }
    [[nodiscard]] lv_indev_t* indev() const { return indev_; }
    [[nodiscard]] lv_group_t* group() const { return group_; }

    // The action a back request performs. It runs in the LVGL context, because
    // that is where the UI may be touched.
    using BackSink = void (*)(void* context);

    // A screen may drive its own rotation and press - the Hall's card list does -
    // and returns true when it did. When it does not, the rotation moves the
    // focus and the press activates the focused widget instead.
    using RotationSink = bool (*)(void* context, int32_t rotation_steps);
    using ConfirmSink = bool (*)(void* context);

    // One detent of rotation. Positive steps move forward through the group.
    // Safe to call from any task.
    void AddRotation(int32_t steps) {
        if (indev_ == nullptr || steps == 0) {
            return;
        }
        pending_steps_.fetch_add(steps, std::memory_order_relaxed);
        WakeReader();
    }

    // Queues one press-and-release pair, which is what LVGL turns into a click
    // on the focused object. The two states are delivered on two consecutive
    // reads, so a caller that resolves a gesture on a single edge - a double
    // click, for instance - still produces a complete click.
    void AddActivation() {
        if (indev_ == nullptr) {
            return;
        }
        activation_pending_.store(true, std::memory_order_release);
        WakeReader();
    }

    // Queues the page's own back action, which the Host UI resolves through the
    // sink below. Safe to call from any task; it never touches the UI itself.
    void AddBack() {
        if (indev_ == nullptr) {
            return;
        }
        back_pending_.store(true, std::memory_order_release);
        WakeReader();
    }

    // Binds what a back request does. Set it while a single task owns the router
    // - before the first gesture - so no lock is needed.
    void SetBackSink(BackSink sink, void* context) {
        back_sink_ = sink;
        back_context_ = context;
    }

    // Binds the current screen's own rotation and press consumer. Same rule: set
    // it from one task, before the first gesture.
    void SetScreenSinks(RotationSink rotate, ConfirmSink confirm, void* context) {
        rotation_sink_ = rotate;
        confirm_sink_ = confirm;
        screen_context_ = context;
    }

   private:
    static void Read(lv_indev_t* indev, lv_indev_data_t* data) {
        auto* router = static_cast<HostEncoderRouter*>(lv_indev_get_user_data(indev));
        if (router == nullptr || data == nullptr) {
            return;
        }
        // Read() runs in the LVGL context, which is what lets the focus be moved
        // from here; handing LVGL a diff instead would move it relative to the
        // whole group rather than to the active screen.
        data->enc_diff = 0;
        const int32_t steps = router->pending_steps_.exchange(0, std::memory_order_relaxed);
        if (steps != 0 && !router->ConsumeRotation(steps)) {
            router->MoveFocus(steps);
        }
        if (router->back_pending_.exchange(false, std::memory_order_acq_rel) && router->back_sink_ != nullptr) {
            // The sink runs the page's back button, so it may leave the page and
            // invalidate objects. Nothing after it may assume the old screen.
            router->back_sink_(router->back_context_);
        }
        if (router->activation_pending_.load(std::memory_order_acquire)) {
            // Touch can change screens without the knob turning, and a click on
            // the widget that was focused before that would act on something
            // the user cannot see.
            router->EnsureFocusOnActiveScreen();
            router->activation_pending_.store(false, std::memory_order_release);
            if (router->ConsumeConfirm()) {
                // The screen acted on its own selection, so there is no widget
                // click to synthesise.
                data->state = LV_INDEV_STATE_RELEASED;
                return;
            }
            router->activation_gap_pending_.store(true, std::memory_order_release);
            data->state = LV_INDEV_STATE_PRESSED;
            // Let LVGL read again straight away so the release follows the press
            // inside the same pass instead of waiting for the next tick.
            data->continue_reading = true;
            return;
        }
        if (router->activation_gap_pending_.load(std::memory_order_acquire)) {
            router->activation_gap_pending_.store(false, std::memory_order_release);
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        data->state = LV_INDEV_STATE_RELEASED;
    }

    [[nodiscard]] bool ConsumeRotation(int32_t steps) const {
        return rotation_sink_ != nullptr && rotation_sink_(screen_context_, steps);
    }

    [[nodiscard]] bool ConsumeConfirm() const { return confirm_sink_ != nullptr && confirm_sink_(screen_context_); }

    void WakeReader() {
        if (indev_ != nullptr && esp_lv_adapter_lock(-1) == ESP_OK) {
            lv_timer_t* read_timer = lv_indev_get_read_timer(indev_);
            if (read_timer != nullptr) {
                lv_timer_resume(read_timer);
                lv_timer_ready(read_timer);
            }
            esp_lv_adapter_unlock();
        }
        (void)esp_lv_adapter_request_wake();
    }

    [[nodiscard]] lv_obj_t* ActiveScreen() const {
        return display_ == nullptr ? nullptr : lv_display_get_screen_active(display_);
    }

    // A widget is navigable when it belongs to the active screen and neither it
    // nor any container above it - up to that screen - is hidden or disabled.
    [[nodiscard]] static bool IsNavigable(lv_obj_t* object, lv_obj_t* active) {
        if (object == nullptr || active == nullptr) {
            return false;
        }
        if (lv_obj_is_disabled(object)) {
            return false;
        }
        for (lv_obj_t* walk = object; walk != nullptr; walk = lv_obj_get_parent(walk)) {
            if (lv_obj_is_hidden(walk)) {
                return false;
            }
            if (walk == active) {
                return true;
            }
        }
        return false;
    }

    // Advances the focus by `steps` navigable widgets, wrapping at both ends.
    // With nothing focused yet it starts at the first widget going forward and
    // at the last one going back, so the first click always shows an outline.
    void MoveFocus(int32_t steps) {
        if (group_ == nullptr) {
            return;
        }
        lv_obj_t* const active = ActiveScreen();
        if (active == nullptr) {
            return;
        }
        const uint32_t count = lv_group_get_obj_count(group_);
        lv_obj_t* const focused = lv_group_get_focused(group_);
        uint32_t navigable = 0U;
        uint32_t focused_slot = UINT32_MAX;
        for (uint32_t index = 0U; index < count; ++index) {
            lv_obj_t* const object = lv_group_get_obj_by_index(group_, index);
            if (!IsNavigable(object, active)) {
                continue;
            }
            if (object == focused) {
                focused_slot = navigable;
            }
            ++navigable;
        }
        if (navigable == 0U) {
            return;
        }
        int32_t slot = focused_slot == UINT32_MAX ? (steps > 0 ? 0 : -1) : static_cast<int32_t>(focused_slot) + steps;
        const int32_t total = static_cast<int32_t>(navigable);
        slot %= total;
        if (slot < 0) {
            slot += total;
        }
        uint32_t seen = 0U;
        for (uint32_t index = 0U; index < count; ++index) {
            lv_obj_t* const object = lv_group_get_obj_by_index(group_, index);
            if (!IsNavigable(object, active)) {
                continue;
            }
            if (seen == static_cast<uint32_t>(slot)) {
                lv_group_focus_obj(object);
                // LVGL's group does not scroll the focus into view (this version
                // has no scroll-on-focus handling at all), so on a scrollable
                // page - the settings lists, the Wi-Fi list - the outline would
                // walk off the screen and rotation would look like it does
                // nothing. Scrolling here is what makes the wheel follow the
                // list, and it is a no-op for widgets whose ancestors cannot
                // scroll, such as the Hall's header buttons.
                lv_obj_scroll_to_view(object, LV_ANIM_ON);
                return;
            }
            ++seen;
        }
    }

    void EnsureFocusOnActiveScreen() {
        lv_obj_t* const active = ActiveScreen();
        if (group_ == nullptr || active == nullptr) {
            return;
        }
        if (IsNavigable(lv_group_get_focused(group_), active)) {
            return;
        }
        MoveFocus(1);
    }

    lv_display_t* display_{};
    lv_indev_t* indev_{};
    lv_group_t* group_{};
    std::atomic<int32_t> pending_steps_{};
    std::atomic<bool> activation_pending_{};
    std::atomic<bool> activation_gap_pending_{};
    std::atomic<bool> back_pending_{};
    BackSink back_sink_{};
    void* back_context_{};
    RotationSink rotation_sink_{};
    ConfirmSink confirm_sink_{};
    void* screen_context_{};
};

}  // namespace micropixel::platform::lvgl

#endif  // MICROPIXEL_PLATFORM_LVGL_HOST_ENCODER_ROUTER_HPP
