#ifndef MICROPIXEL_SDK_UI_BUTTON_HPP
#define MICROPIXEL_SDK_UI_BUTTON_HPP

#include <stdint.h>

#include "sdk/event.hpp"
#include "sdk/fixed_string.hpp"
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

// Labels use a centered x anchor, while y is the top of the complete line
// box. Keeping this calculation beside the headless interaction primitive
// lets every visual Button variant share identical two-axis centering.
[[nodiscard]] inline Result<Point> CenteredLabelPosition(Rect bounds, const TextMetrics& metrics) {
    if (bounds.empty() || metrics.width == 0U || metrics.height == 0U ||
        metrics.width > static_cast<uint32_t>(bounds.width) || metrics.height > static_cast<uint32_t>(bounds.height) ||
        metrics.height > static_cast<uint32_t>(INT32_MAX)) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    const int64_t center_x = static_cast<int64_t>(bounds.x) + bounds.width / 2;
    const int64_t top = static_cast<int64_t>(bounds.y) +
                        (static_cast<int64_t>(bounds.height) - static_cast<int64_t>(metrics.height)) / 2;
    if (center_x < INT32_MIN || center_x > INT32_MAX || top < INT32_MIN || top > INT32_MAX) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    return Point{static_cast<int32_t>(center_x), static_cast<int32_t>(top)};
}

class Button final {
   public:
    static constexpr uint32_t kDiagnosticBytes = 256U;

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
    [[nodiscard]] FixedString<kDiagnosticBytes> ToString() const {
        FixedString<kDiagnosticBytes> description;
        const Rect hit = hit_bounds();
        description.Append("Button bounds=(x=");
        description.AppendInt(bounds_.x);
        description.Append(",y=");
        description.AppendInt(bounds_.y);
        description.Append(",w=");
        description.AppendInt(bounds_.width);
        description.Append(",h=");
        description.AppendInt(bounds_.height);
        description.Append(") hit_bounds=(x=");
        description.AppendInt(hit.x);
        description.Append(",y=");
        description.AppendInt(hit.y);
        description.Append(",w=");
        description.AppendInt(hit.width);
        description.Append(",h=");
        description.AppendInt(hit.height);
        description.Append(") enabled=");
        description.Append(enabled_ ? "true" : "false");
        description.Append(" tracking=");
        description.Append(tracking_ ? "true" : "false");
        description.Append(" pressed=");
        description.Append(pressed_ ? "true" : "false");
        description.Append(" touch_id=");
        description.AppendUint(touch_id_);
        return description;
    }

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
