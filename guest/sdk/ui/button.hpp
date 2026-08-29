#ifndef MICROPIXEL_SDK_UI_BUTTON_HPP
#define MICROPIXEL_SDK_UI_BUTTON_HPP

#include <stdint.h>

#include "sdk/event.hpp"
#include "sdk/graphics.hpp"

namespace micropixel::ui {

// Button owns only touch interaction state. The app owns the action and binds
// pressed/enabled state to retained Scene nodes.
struct ButtonUpdate final {
    bool handled{};
    bool visual_changed{};
    bool clicked{};

    [[nodiscard]] constexpr bool redraw() const { return visual_changed || clicked; }
};

class Button final {
   public:
    constexpr Button() = default;
    explicit constexpr Button(Rect bounds, uint16_t hit_padding = 0U) : bounds_(bounds), hit_padding_(hit_padding) {}

    void SetBounds(Rect bounds) {
        if (bounds_.x == bounds.x && bounds_.y == bounds.y && bounds_.width == bounds.width &&
            bounds_.height == bounds.height) {
            return;
        }
        bounds_ = bounds;
        Reset();
    }

    void SetHitPadding(uint16_t hit_padding) {
        if (hit_padding_ == hit_padding) {
            return;
        }
        hit_padding_ = hit_padding;
        Reset();
    }

    void SetEnabled(bool enabled) {
        enabled_ = enabled;
        if (!enabled_) {
            Reset();
        }
    }

    void Reset() {
        tracking_ = false;
        pressed_ = false;
        touch_id_ = 0U;
    }

    [[nodiscard]] ButtonUpdate OnTouch(const TouchEvent& event) {
        if (!enabled_) {
            return {};
        }

        const bool inside = hit_bounds().contains(event.position());
        if (event.phase() == TouchPhase::kDown) {
            if (tracking_ || !inside) {
                return {};
            }
            tracking_ = true;
            pressed_ = true;
            touch_id_ = event.id();
            return {true, true, false};
        }
        if (!tracking_ || event.id() != touch_id_) {
            return {};
        }
        if (event.phase() == TouchPhase::kMove) {
            const bool changed = pressed_ != inside;
            pressed_ = inside;
            return {true, changed, false};
        }
        if (event.phase() != TouchPhase::kUp && event.phase() != TouchPhase::kCancel) {
            return {true, false, false};
        }

        const bool was_pressed = pressed_;
        const bool clicked = event.phase() == TouchPhase::kUp && inside;
        Reset();
        return {true, was_pressed, clicked};
    }

    [[nodiscard]] constexpr Rect bounds() const { return bounds_; }
    [[nodiscard]] constexpr Rect hit_bounds() const { return bounds_.inset(-static_cast<int32_t>(hit_padding_)); }
    [[nodiscard]] constexpr uint16_t hit_padding() const { return hit_padding_; }
    [[nodiscard]] constexpr bool enabled() const { return enabled_; }
    [[nodiscard]] constexpr bool tracking() const { return tracking_; }
    [[nodiscard]] constexpr bool pressed() const { return pressed_; }

   private:
    Rect bounds_{};
    uint32_t touch_id_{};
    uint16_t hit_padding_{};
    bool enabled_{true};
    bool tracking_{};
    bool pressed_{};
};

}  // namespace micropixel::ui

#endif
