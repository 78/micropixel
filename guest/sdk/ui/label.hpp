#ifndef MICROPIXEL_SDK_UI_LABEL_HPP
#define MICROPIXEL_SDK_UI_LABEL_HPP

#include "sdk/fixed_string.hpp"
#include "sdk/graphics.hpp"
#include "sdk/panic.hpp"
#include "sdk/scene.hpp"
#include "sdk/ui/text_overflow.hpp"

namespace micropixel::ui {

enum class HorizontalAlignment : uint8_t { kStart, kCenter, kEnd };
enum class VerticalAlignment : uint8_t { kStart, kCenter, kEnd };

struct LabelStyle final {
    Color color{Color::White()};
    SystemFont font{SystemFont::kMedium};
    HorizontalAlignment horizontal{HorizontalAlignment::kCenter};
    VerticalAlignment vertical{VerticalAlignment::kCenter};
};

class Label final {
   public:
    static constexpr uint32_t kMaxTextBytes = 128U;
    static constexpr uint32_t kDiagnosticBytes = kTextOverflowDiagnosticBytes;
    constexpr Label() = default;
    Label(const Label&) = delete;
    Label& operator=(const Label&) = delete;
    constexpr Label(Label&&) noexcept = default;
    constexpr Label& operator=(Label&&) noexcept = default;

    [[nodiscard]] static Label CreateIn(Container& parent, const char* text, LabelStyle style = {}) {
        FixedString<kMaxTextBytes + 1U> copied;
        Assert(CopyText(text, copied), "label text invalid");
        const Renderer renderer{Renderer::CapabilityToken{}};
        auto measured = renderer.MeasureText(copied.c_str(), style.font);
        Assert(measured.has_value(), "label measurement failed");
        Label result;
        result.node_ = parent.CreateLabel({0, 0}, copied.c_str(), style.color, style.font,
                                          style.horizontal == HorizontalAlignment::kCenter);
        result.text_ = copied;
        result.metrics_ = measured.value();
        result.style_ = style;
        return result;
    }

    [[nodiscard]] Size intrinsic_size() const { return {metrics_.width, metrics_.height}; }
    [[nodiscard]] Rect bounds() const { return bounds_; }
    [[nodiscard]] FixedString<kDiagnosticBytes> ToString() const {
        FixedString<kDiagnosticBytes> description;
        description.Append("Label bounds=(x=");
        description.AppendInt(bounds_.x);
        description.Append(",y=");
        description.AppendInt(bounds_.y);
        description.Append(",w=");
        description.AppendInt(bounds_.width);
        description.Append(",h=");
        description.AppendInt(bounds_.height);
        description.Append(") measured=(w=");
        description.AppendUint(metrics_.width);
        description.Append(",h=");
        description.AppendUint(metrics_.height);
        description.Append(") horizontal=");
        description.Append(HorizontalAlignmentName(style_.horizontal));
        description.Append(" vertical=");
        description.Append(VerticalAlignmentName(style_.vertical));
        description.Append(" text=[");
        detail::AppendDiagnosticText(description, text_.c_str());
        description.Append("]");
        return description;
    }

    [[nodiscard]] Result<void> SetBounds(SceneUpdate& update, Rect bounds) {
        if (bounds == bounds_) {
            return {};
        }
        auto position = Position(bounds, metrics_, style_);
        if (!position.has_value()) {
            return unexpected(position.error());
        }
        bounds_ = bounds;
        node_.SetCentered(update, style_.horizontal == HorizontalAlignment::kCenter);
        node_.SetPosition(update, position.value());
        return {};
    }

    [[nodiscard]] Result<void> SetText(SceneUpdate& update, const char* text) {
        FixedString<kMaxTextBytes + 1U> copied;
        if (!CopyText(text, copied)) {
            return unexpected(Error{ErrorCode::kInvalidArgument});
        }
        if (SameText(copied, text_)) {
            return {};
        }
        const Renderer renderer{Renderer::CapabilityToken{}};
        auto measured = renderer.MeasureText(copied.c_str(), style_.font);
        if (!measured.has_value()) {
            return unexpected(measured.error());
        }
        if (!bounds_.empty()) {
            auto position = Position(bounds_, measured.value(), style_);
            if (position.has_value()) {
                node_.SetPosition(update, position.value());
            } else {
                // Auto-layout containers may need to grow this Label after its
                // content changes. Mark the previous allocation stale and let
                // the containing Flex/Grid provide the new bounds below in the
                // same SceneUpdate.
                bounds_ = {};
            }
        }
        text_ = copied;
        metrics_ = measured.value();
        node_.SetText(update, text_.c_str());
        return {};
    }

    void SetColor(SceneUpdate& update, Color color) {
        if (color == style_.color) {
            return;
        }
        style_.color = color;
        node_.SetColor(update, color);
    }

    void SetVisible(SceneUpdate& update, bool visible) { node_.SetVisible(update, visible); }

   private:
    [[nodiscard]] static constexpr const char* HorizontalAlignmentName(HorizontalAlignment alignment) {
        switch (alignment) {
            case HorizontalAlignment::kStart:
                return "start";
            case HorizontalAlignment::kCenter:
                return "center";
            case HorizontalAlignment::kEnd:
                return "end";
        }
        return "unknown";
    }

    [[nodiscard]] static constexpr const char* VerticalAlignmentName(VerticalAlignment alignment) {
        switch (alignment) {
            case VerticalAlignment::kStart:
                return "start";
            case VerticalAlignment::kCenter:
                return "center";
            case VerticalAlignment::kEnd:
                return "end";
        }
        return "unknown";
    }

    static bool CopyText(const char* source, FixedString<kMaxTextBytes + 1U>& destination) {
        destination.Clear();
        return source != nullptr && source[0] != '\0' && destination.Append(source);
    }

    static bool SameText(const FixedString<kMaxTextBytes + 1U>& left, const FixedString<kMaxTextBytes + 1U>& right) {
        if (left.size() != right.size()) {
            return false;
        }
        for (uint32_t index = 0U; index < left.size(); ++index) {
            if (left.c_str()[index] != right.c_str()[index]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static Result<Point> Position(Rect bounds, TextMetrics metrics, LabelStyle style) {
        if (bounds.empty() || metrics.width > static_cast<uint32_t>(bounds.width) ||
            metrics.height > static_cast<uint32_t>(bounds.height)) {
            return unexpected(Error{ErrorCode::kResourceExhausted});
        }
        int32_t x = bounds.x;
        if (style.horizontal == HorizontalAlignment::kCenter) {
            x = bounds.center_x();
        } else if (style.horizontal == HorizontalAlignment::kEnd) {
            x = bounds.x + bounds.width - static_cast<int32_t>(metrics.width);
        }
        int32_t y = bounds.y;
        if (style.vertical == VerticalAlignment::kCenter) {
            y += (bounds.height - static_cast<int32_t>(metrics.height)) / 2;
        } else if (style.vertical == VerticalAlignment::kEnd) {
            y += bounds.height - static_cast<int32_t>(metrics.height);
        }
        return Point{x, y};
    }

    LabelNode node_{};
    FixedString<kMaxTextBytes + 1U> text_{};
    TextMetrics metrics_{};
    LabelStyle style_{};
    Rect bounds_{};
};

}  // namespace micropixel::ui

#endif
