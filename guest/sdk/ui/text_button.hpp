#ifndef MICROPIXEL_SDK_UI_TEXT_BUTTON_HPP
#define MICROPIXEL_SDK_UI_TEXT_BUTTON_HPP

#include <stdint.h>

#include "sdk/fixed_string.hpp"
#include "sdk/log.hpp"
#include "sdk/panic.hpp"
#include "sdk/ui/text_overflow.hpp"

namespace micropixel::ui {

struct TextButtonStyle final {
    Color background{Color::Black()};
    Color text{Color::White()};
    Color feedback{Color::Black()};
    SystemFont font{SystemFont::kMedium};
    uint32_t corner_radius{16U};
    uint8_t pressed_feedback_opacity{48U};
    uint8_t disabled_feedback_opacity{112U};
};

struct TextButtonProperties final {
    Rect bounds{};
    const char* text{};
    TextButtonStyle style{};
    TextOverflow overflow{TextOverflow::kClip};
    uint16_t hit_padding{};
    bool enabled{true};
};

// Allocation-free retained text button. It composes the headless Button with
// one background RoundedRect, one feedback RoundedRect, and one horizontally anchored
// Label. Text metrics are cached so vertical centering never enters the frame
// hot path.
class TextButton final {
   public:
    static constexpr uint32_t kMaxTextBytes = 128U;
    static constexpr uint32_t kDiagnosticBytes = kTextOverflowDiagnosticBytes;

    constexpr TextButton() = default;
    TextButton(const TextButton&) = delete;
    TextButton& operator=(const TextButton&) = delete;
    constexpr TextButton(TextButton&&) noexcept = default;
    constexpr TextButton& operator=(TextButton&&) noexcept = default;

   private:
    [[nodiscard]] static TextButton CreateIn(Container& parent, const TextButtonProperties& properties) {
        FixedString<kMaxTextBytes + 1U> text;
        Assert(CopyText(properties.text, text) && !properties.bounds.empty(), "text button properties invalid");
        const Renderer renderer{Renderer::CapabilityToken{}};
        auto measured = renderer.MeasureText(text.c_str(), properties.style.font);
        Assert(measured.has_value(), "text button measurement failed");
        const Rect local_bounds{0, 0, properties.bounds.width, properties.bounds.height};
        auto position = CenteredLabelPosition(local_bounds, measured.value(), properties.overflow);
        if (!position.has_value()) {
            if (TextExceedsBounds(local_bounds, measured.value())) {
                auto diagnostic =
                    FormatOverflowDiagnostic("rejected", properties.bounds, measured.value(), text.c_str(),
                                             properties.overflow, true);
                Panic(diagnostic.c_str());
            }
            auto diagnostic =
                FormatOverflowDiagnostic("invalid-layout", properties.bounds, measured.value(), text.c_str(),
                                         properties.overflow, TextExceedsBounds(local_bounds, measured.value()));
            Panic(diagnostic.c_str());
        }

        TextButton result;
        result.interaction_ = Button{local_bounds, properties.hit_padding};
        result.interaction_.SetEnabled(properties.enabled);
        result.style_ = properties.style;
        result.overflow_ = properties.overflow;
        result.bounds_ = properties.bounds;
        result.container_ = parent.CreateContainer(
            {.clip = local_bounds, .translation = {properties.bounds.x, properties.bounds.y}});
        result.background_ = result.container_.CreateRoundedRect(
            local_bounds, {.fill = properties.style.background, .radius = properties.style.corner_radius});
        result.feedback_ = result.container_.CreateRoundedRect(local_bounds, {.fill = properties.style.feedback,
                                                                              .radius = properties.style.corner_radius,
                                                                              .opacity = result.FeedbackOpacity()});
        result.label_ = result.container_.CreateLabel(position.value(), text.c_str(), properties.style.text,
                                                      properties.style.font, true);
        result.text_ = text;
        result.metrics_ = measured.value();
        result.UpdateClipped(TextExceedsBounds(local_bounds, result.metrics_));
        return result;
    }

   public:
    [[nodiscard]] static Result<Point> CenteredLabelPosition(Rect bounds, const TextMetrics& metrics) {
        return micropixel::ui::CenteredLabelPosition(bounds, metrics);
    }

    [[nodiscard]] static Result<Point> CenteredLabelPosition(Rect bounds, const TextMetrics& metrics,
                                                             TextOverflow overflow) {
        return TextPosition(bounds, metrics, overflow);
    }

    [[nodiscard]] static bool TextExceedsBounds(Rect bounds, const TextMetrics& metrics) {
        return micropixel::ui::TextExceedsBounds(bounds, metrics);
    }

    [[nodiscard]] static FixedString<kDiagnosticBytes> FormatOverflowDiagnostic(const char* action, Rect bounds,
                                                                                const TextMetrics& metrics,
                                                                                const char* text,
                                                                                TextOverflow overflow,
                                                                                bool clipped) {
        return FormatTextOverflowDiagnostic("TextButton", action, bounds, metrics, text, overflow, clipped);
    }

    [[nodiscard]] bool valid() const { return container_.valid(); }
    [[nodiscard]] Size intrinsic_size() const {
        return {static_cast<uint32_t>(bounds_.width), static_cast<uint32_t>(bounds_.height)};
    }

    void Destroy(SceneUpdate& update) { container_.Destroy(update); }

    [[nodiscard]] ButtonUpdate OnTouch(const TouchEvent& event) {
        return valid() ? interaction_.OnTouch(event.WithPosition(container_.ToLocal(event.position())))
                       : ButtonUpdate{};
    }

    [[nodiscard]] ButtonUpdate OnTouch(SceneUpdate& update, const TouchEvent& event) {
        if (!valid()) {
            return {};
        }
        const ButtonUpdate result = interaction_.OnTouch(event.WithPosition(container_.ToLocal(event.position())));
        if (result.visual_changed) {
            Sync(update);
        }
        return result;
    }

    // Apply interaction-driven opacity and visibility to retained nodes. Apps
    // normally call this in the same SceneUpdate used for the rest of a frame.
    void Sync(SceneUpdate& update) {
        container_.SetVisible(update, visible_);
        const uint8_t opacity = FeedbackOpacity();
        feedback_.SetOpacity(update, opacity);
        feedback_.SetVisible(update, opacity != 0U);
    }

    [[nodiscard]] Result<void> SetBounds(SceneUpdate& update, Rect bounds) {
        if (bounds.x == bounds_.x && bounds.y == bounds_.y && bounds.width == bounds_.width &&
            bounds.height == bounds_.height) {
            return {};
        }
        const Rect local_bounds{0, 0, bounds.width, bounds.height};
        auto position = CenteredLabelPosition(local_bounds, metrics_, overflow_);
        if (!position.has_value()) {
            WarnRejectedOverflow(bounds, metrics_, text_.c_str());
            return unexpected(position.error());
        }
        bounds_ = bounds;
        interaction_.SetBounds(local_bounds);
        container_.SetClip(update, local_bounds);
        container_.SetTranslation(update, {bounds.x, bounds.y});
        background_.SetRect(update, local_bounds);
        feedback_.SetRect(update, local_bounds);
        label_.SetPosition(update, position.value());
        UpdateClipped(TextExceedsBounds(local_bounds, metrics_));
        Sync(update);
        return {};
    }

    [[nodiscard]] Result<void> SetText(SceneUpdate& update, const char* text) {
        FixedString<kMaxTextBytes + 1U> candidate;
        if (!CopyText(text, candidate)) {
            return unexpected(Error{ErrorCode::kInvalidArgument});
        }
        if (SameText(candidate, text_)) {
            return {};
        }
        const Renderer renderer{Renderer::CapabilityToken{}};
        auto measured = renderer.MeasureText(candidate.c_str(), style_.font);
        if (!measured.has_value()) {
            return unexpected(measured.error());
        }
        auto position = CenteredLabelPosition(interaction_.bounds(), measured.value(), overflow_);
        if (!position.has_value()) {
            WarnRejectedOverflow(bounds_, measured.value(), candidate.c_str());
            return unexpected(position.error());
        }
        text_ = candidate;
        metrics_ = measured.value();
        label_.SetText(update, text_.c_str());
        label_.SetPosition(update, position.value());
        UpdateClipped(TextExceedsBounds(interaction_.bounds(), metrics_));
        return {};
    }

    [[nodiscard]] Result<void> SetStyle(SceneUpdate& update, TextButtonStyle style) {
        const Renderer renderer{Renderer::CapabilityToken{}};
        auto measured = renderer.MeasureText(text_.c_str(), style.font);
        if (!measured.has_value()) {
            return unexpected(measured.error());
        }
        auto position = CenteredLabelPosition(interaction_.bounds(), measured.value(), overflow_);
        if (!position.has_value()) {
            WarnRejectedOverflow(bounds_, measured.value(), text_.c_str());
            return unexpected(position.error());
        }
        style_ = style;
        metrics_ = measured.value();
        background_.SetFillColor(update, style_.background);
        background_.SetRadius(update, style_.corner_radius);
        feedback_.SetFillColor(update, style_.feedback);
        feedback_.SetRadius(update, style_.corner_radius);
        label_.SetColor(update, style_.text);
        label_.SetFont(update, style_.font);
        label_.SetPosition(update, position.value());
        UpdateClipped(TextExceedsBounds(interaction_.bounds(), metrics_));
        Sync(update);
        return {};
    }

    void SetEnabled(SceneUpdate& update, bool enabled) {
        if (interaction_.enabled() == enabled) {
            return;
        }
        interaction_.SetEnabled(enabled);
        Sync(update);
    }

    void SetVisible(SceneUpdate& update, bool visible) {
        visible_ = visible;
        if (!visible_) {
            interaction_.Reset();
        }
        Sync(update);
    }

    void Reset(SceneUpdate& update) {
        interaction_.Reset();
        Sync(update);
    }

    void SetHitPadding(SceneUpdate& update, uint16_t hit_padding) {
        interaction_.SetHitPadding(hit_padding);
        Sync(update);
    }

    [[nodiscard]] constexpr Rect bounds() const { return bounds_; }
    [[nodiscard]] constexpr bool enabled() const { return interaction_.enabled(); }
    [[nodiscard]] constexpr bool pressed() const { return interaction_.pressed(); }
    [[nodiscard]] bool visible() const { return valid() && visible_; }
    [[nodiscard]] constexpr const char* text() const { return text_.c_str(); }
    [[nodiscard]] constexpr TextMetrics text_metrics() const { return metrics_; }
    [[nodiscard]] constexpr TextButtonStyle style() const { return style_; }
    [[nodiscard]] constexpr TextOverflow overflow() const { return overflow_; }
    [[nodiscard]] constexpr bool text_clipped() const { return text_clipped_; }
    [[nodiscard]] FixedString<kDiagnosticBytes> ToString() const {
        return FormatTextControlDescription("TextButton", bounds_, metrics_, text_.c_str(), overflow_, text_clipped_);
    }

   private:
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

    [[nodiscard]] uint8_t FeedbackOpacity() const {
        return !interaction_.enabled()  ? style_.disabled_feedback_opacity
               : interaction_.pressed() ? style_.pressed_feedback_opacity
                                        : 0U;
    }

    void UpdateClipped(bool clipped) {
        text_clipped_ = clipped;
        if (overflow_ == TextOverflow::kClip && clipped && !overflow_warning_emitted_) {
            const Log log{Log::CapabilityToken{}};
            const auto diagnostic =
                FormatOverflowDiagnostic("clipped", bounds_, metrics_, text_.c_str(), overflow_, text_clipped_);
            log.Warning(diagnostic.c_str());
            overflow_warning_emitted_ = true;
        }
    }

    void WarnRejectedOverflow(Rect bounds, const TextMetrics& metrics, const char* text) {
        if (overflow_ != TextOverflow::kReject || overflow_warning_emitted_ ||
            !TextExceedsBounds({0, 0, bounds.width, bounds.height}, metrics)) {
            return;
        }
        const Log log{Log::CapabilityToken{}};
        const auto diagnostic = FormatOverflowDiagnostic("rejected", bounds, metrics, text, overflow_, true);
        log.Warning(diagnostic.c_str());
        overflow_warning_emitted_ = true;
    }

    Button interaction_{};
    ContainerNode container_{};
    RoundedRectNode background_{};
    RoundedRectNode feedback_{};
    LabelNode label_{};
    FixedString<kMaxTextBytes + 1U> text_{};
    TextMetrics metrics_{};
    TextButtonStyle style_{};
    TextOverflow overflow_{TextOverflow::kClip};
    Rect bounds_{};
    bool visible_{true};
    bool text_clipped_{};
    bool overflow_warning_emitted_{};

    friend class micropixel::Container;
};

}  // namespace micropixel::ui

namespace micropixel {

inline ui::TextButton Container::CreateTextButton(const ui::TextButtonProperties& properties) {
    return ui::TextButton::CreateIn(*this, properties);
}

}  // namespace micropixel

#endif
