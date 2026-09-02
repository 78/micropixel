#ifndef MICROPIXEL_SDK_UI_IMAGE_BUTTON_HPP
#define MICROPIXEL_SDK_UI_IMAGE_BUTTON_HPP

#include <stdint.h>

#include "sdk/fixed_string.hpp"
#include "sdk/log.hpp"
#include "sdk/panic.hpp"
#include "sdk/resources.hpp"
#include "sdk/ui/text_overflow.hpp"

namespace micropixel::ui {

struct ImageButtonStyle final {
    Color text{Color::Black()};
    SystemFont font{SystemFont::kMedium};
    uint8_t normal_image_opacity{255U};
    uint8_t pressed_image_opacity{160U};
    uint8_t disabled_image_opacity{96U};

    friend constexpr bool operator==(ImageButtonStyle, ImageButtonStyle) = default;
};

struct ImageButtonProperties final {
    Rect bounds{};
    Rect source{};
    const char* text{};
    ImageButtonStyle style{};
    TextOverflow overflow{TextOverflow::kClip};
    uint16_t hit_padding{};
    bool enabled{true};
};

// Retained image-backed Button. The image may be scaled independently from
// its source rectangle, while the Label is measured and centered in the final
// destination bounds. This keeps App code free of font-height assumptions.
class ImageButton final {
   public:
    static constexpr uint32_t kMaxTextBytes = 128U;
    static constexpr uint32_t kDiagnosticBytes = kTextOverflowDiagnosticBytes;

    constexpr ImageButton() = default;
    ImageButton(const ImageButton&) = delete;
    ImageButton& operator=(const ImageButton&) = delete;
    constexpr ImageButton(ImageButton&&) noexcept = default;
    constexpr ImageButton& operator=(ImageButton&&) noexcept = default;

   private:
    [[nodiscard]] static ImageButton CreateIn(Container& parent, const Texture& texture,
                                              const ImageButtonProperties& properties) {
        FixedString<kMaxTextBytes + 1U> text;
        Assert(texture.valid() && ValidSource(texture, properties.source) && CopyText(properties.text, text) &&
                   !properties.bounds.empty(),
               "image button properties invalid");
        const Renderer renderer{Renderer::CapabilityToken{}};
        auto measured = renderer.MeasureText(text.c_str(), properties.style.font);
        Assert(measured.has_value(), "image button measurement failed");
        const Rect local_bounds{0, 0, properties.bounds.width, properties.bounds.height};
        auto position = TextPosition(local_bounds, measured.value(), properties.overflow);
        if (!position.has_value()) {
            const bool clipped = TextExceedsBounds(local_bounds, measured.value());
            const auto diagnostic = FormatTextOverflowDiagnostic(
                "ImageButton", clipped ? "rejected" : "invalid-layout", properties.bounds, measured.value(),
                text.c_str(), properties.overflow, clipped);
            Panic(diagnostic.c_str());
        }

        ImageButton result;
        result.interaction_ = Button{local_bounds, properties.hit_padding};
        result.interaction_.SetEnabled(properties.enabled);
        result.style_ = properties.style;
        result.overflow_ = properties.overflow;
        result.bounds_ = properties.bounds;
        result.source_ = properties.source;
        result.container_ = parent.CreateContainer(
            {.clip = local_bounds, .translation = {properties.bounds.x, properties.bounds.y}});
        result.image_ = result.container_.CreateSprite(texture, local_bounds, properties.source, result.ImageOpacity());
        result.label_ = result.container_.CreateLabel(position.value(), text.c_str(), properties.style.text,
                                                      properties.style.font, true);
        result.text_ = text;
        result.metrics_ = measured.value();
        result.UpdateClipped(TextExceedsBounds(local_bounds, result.metrics_));
        return result;
    }

   public:
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

    void Sync(SceneUpdate& update) {
        container_.SetVisible(update, visible_);
        image_.SetOpacity(update, ImageOpacity());
    }

    [[nodiscard]] Result<void> SetBounds(SceneUpdate& update, Rect bounds) {
        if (bounds == bounds_) {
            return {};
        }
        const Rect local_bounds{0, 0, bounds.width, bounds.height};
        auto position = TextPosition(local_bounds, metrics_, overflow_);
        if (!position.has_value()) {
            WarnRejectedOverflow(bounds, metrics_, text_.c_str());
            return unexpected(position.error());
        }
        bounds_ = bounds;
        interaction_.SetBounds(local_bounds);
        container_.SetClip(update, local_bounds);
        container_.SetTranslation(update, {bounds.x, bounds.y});
        image_.SetDestination(update, local_bounds);
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
        auto position = TextPosition(interaction_.bounds(), measured.value(), overflow_);
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

    [[nodiscard]] Result<void> SetStyle(SceneUpdate& update, ImageButtonStyle style) {
        if (style == style_) {
            return {};
        }
        const Renderer renderer{Renderer::CapabilityToken{}};
        auto measured = renderer.MeasureText(text_.c_str(), style.font);
        if (!measured.has_value()) {
            return unexpected(measured.error());
        }
        auto position = TextPosition(interaction_.bounds(), measured.value(), overflow_);
        if (!position.has_value()) {
            WarnRejectedOverflow(bounds_, measured.value(), text_.c_str());
            return unexpected(position.error());
        }
        style_ = style;
        metrics_ = measured.value();
        label_.SetColor(update, style_.text);
        label_.SetFont(update, style_.font);
        label_.SetPosition(update, position.value());
        UpdateClipped(TextExceedsBounds(interaction_.bounds(), metrics_));
        Sync(update);
        return {};
    }

    [[nodiscard]] Result<void> SetImage(SceneUpdate& update, const Texture& texture, Rect source) {
        if (!texture.valid() || !ValidSource(texture, source)) {
            return unexpected(Error{ErrorCode::kInvalidArgument});
        }
        source_ = source;
        image_.SetTexture(update, texture);
        image_.SetSource(update, source);
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
    [[nodiscard]] constexpr Rect source() const { return source_; }
    [[nodiscard]] constexpr bool enabled() const { return interaction_.enabled(); }
    [[nodiscard]] constexpr bool pressed() const { return interaction_.pressed(); }
    [[nodiscard]] bool visible() const { return valid() && visible_; }
    [[nodiscard]] constexpr const char* text() const { return text_.c_str(); }
    [[nodiscard]] constexpr TextMetrics text_metrics() const { return metrics_; }
    [[nodiscard]] constexpr ImageButtonStyle style() const { return style_; }
    [[nodiscard]] constexpr TextOverflow overflow() const { return overflow_; }
    [[nodiscard]] constexpr bool text_clipped() const { return text_clipped_; }
    [[nodiscard]] FixedString<kDiagnosticBytes> ToString() const {
        return FormatTextControlDescription("ImageButton", bounds_, metrics_, text_.c_str(), overflow_, text_clipped_);
    }

   private:
    [[nodiscard]] static bool ValidSource(const Texture& texture, Rect source) {
        if (source.empty() || source.x < 0 || source.y < 0) {
            return false;
        }
        const uint64_t right = static_cast<uint64_t>(source.x) + static_cast<uint32_t>(source.width);
        const uint64_t bottom = static_cast<uint64_t>(source.y) + static_cast<uint32_t>(source.height);
        return right <= texture.width() && bottom <= texture.height();
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

    [[nodiscard]] uint8_t ImageOpacity() const {
        return !interaction_.enabled()  ? style_.disabled_image_opacity
               : interaction_.pressed() ? style_.pressed_image_opacity
                                        : style_.normal_image_opacity;
    }

    void UpdateClipped(bool clipped) {
        text_clipped_ = clipped;
        if (overflow_ == TextOverflow::kClip && clipped && !overflow_warning_emitted_) {
            const Log log{Log::CapabilityToken{}};
            const auto diagnostic = FormatTextOverflowDiagnostic("ImageButton", "clipped", bounds_, metrics_,
                                                                 text_.c_str(), overflow_, text_clipped_);
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
        const auto diagnostic =
            FormatTextOverflowDiagnostic("ImageButton", "rejected", bounds, metrics, text, overflow_, true);
        log.Warning(diagnostic.c_str());
        overflow_warning_emitted_ = true;
    }

    Button interaction_{};
    ContainerNode container_{};
    SpriteNode image_{};
    LabelNode label_{};
    FixedString<kMaxTextBytes + 1U> text_{};
    TextMetrics metrics_{};
    ImageButtonStyle style_{};
    TextOverflow overflow_{TextOverflow::kClip};
    Rect bounds_{};
    Rect source_{};
    bool visible_{true};
    bool text_clipped_{};
    bool overflow_warning_emitted_{};

    friend class micropixel::Container;
};

}  // namespace micropixel::ui

namespace micropixel {

inline ui::ImageButton Container::CreateImageButton(const Texture& texture,
                                                    const ui::ImageButtonProperties& properties) {
    return ui::ImageButton::CreateIn(*this, texture, properties);
}

}  // namespace micropixel

#endif
