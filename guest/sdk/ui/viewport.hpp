#ifndef MICROPIXEL_SDK_UI_VIEWPORT_HPP
#define MICROPIXEL_SDK_UI_VIEWPORT_HPP

#include <stdint.h>

#include "sdk/event.hpp"
#include "sdk/graphics.hpp"
#include "sdk/resources.hpp"

namespace micropixel::ui {

// Maps one fixed logical design space onto the physical RendererInfo space.
// Geometry is transformed command-by-command in the Guest, so the wire ABI and
// Host renderer remain resolution-independent.
class Viewport final {
   public:
    constexpr Viewport(RendererInfo physical, Size logical)
        : Viewport(Size{physical.width(), physical.height()}, logical) {}

    constexpr Viewport(Size physical, Size logical) : physical_(physical), logical_(logical) {}

    [[nodiscard]] constexpr Size logical_size() const { return logical_; }
    [[nodiscard]] constexpr Size physical_size() const { return physical_; }
    [[nodiscard]] constexpr bool valid() const {
        return physical_.width > 0 && physical_.height > 0 && logical_.width > 0 && logical_.height > 0;
    }
    [[nodiscard]] constexpr bool identity() const {
        return physical_.width == logical_.width && physical_.height == logical_.height;
    }

    [[nodiscard]] constexpr Point ToPhysical(Point point) const {
        return {ScaleCoordinate(point.x, physical_.width, logical_.width),
                ScaleCoordinate(point.y, physical_.height, logical_.height)};
    }

    [[nodiscard]] constexpr Rect ToPhysical(Rect rect) const {
        const int32_t left = ScaleCoordinate(rect.x, physical_.width, logical_.width);
        const int32_t top = ScaleCoordinate(rect.y, physical_.height, logical_.height);
        const int32_t right = ScaleCoordinate(rect.x + rect.width, physical_.width, logical_.width);
        const int32_t bottom = ScaleCoordinate(rect.y + rect.height, physical_.height, logical_.height);
        return {left, top, right - left, bottom - top};
    }

    [[nodiscard]] constexpr Point DeltaToPhysical(Point delta) const {
        return {ScaleCoordinate(delta.x, physical_.width, logical_.width),
                ScaleCoordinate(delta.y, physical_.height, logical_.height)};
    }

    [[nodiscard]] constexpr Point ToLogical(Point point) const {
        return {ScaleCoordinate(point.x, logical_.width, physical_.width),
                ScaleCoordinate(point.y, logical_.height, physical_.height)};
    }

    [[nodiscard]] constexpr TouchEvent ToLogical(const TouchEvent& event) const {
        return event.WithPosition(ToLogical(event.position()));
    }

    [[nodiscard]] constexpr SystemFont ToPhysical(SystemFont font) const {
        if (physical_.height >= logical_.height) {
            return font;
        }
        switch (font) {
            case SystemFont::kTitle:
                return SystemFont::kLarge;
            case SystemFont::kLarge:
                return SystemFont::kMedium;
            case SystemFont::kMedium:
                return SystemFont::kSmall;
            case SystemFont::kSmall:
                return SystemFont::kSmall;
        }
        return SystemFont::kSmall;
    }

   private:
    [[nodiscard]] static constexpr int32_t ScaleCoordinate(int32_t value, int32_t numerator, int32_t denominator) {
        if (denominator <= 0) {
            return 0;
        }
        const int64_t product = static_cast<int64_t>(value) * numerator;
        const int64_t rounding = denominator / 2;
        return static_cast<int32_t>(product >= 0 ? (product + rounding) / denominator
                                                  : (product - rounding) / denominator);
    }

    Size physical_{};
    Size logical_{};
};

class ViewportFrame final {
   public:
    ViewportFrame(Frame frame, const Viewport& viewport)
        : frame_(static_cast<Frame&&>(frame)), viewport_(viewport) {}

    ViewportFrame(const ViewportFrame&) = delete;
    ViewportFrame& operator=(const ViewportFrame&) = delete;
    ViewportFrame(ViewportFrame&&) = delete;
    ViewportFrame& operator=(ViewportFrame&&) = delete;

    void Clear(Color color) { frame_.Clear(color); }
    void FillRect(Rect rect, Color color, uint8_t opacity = 255U) {
        frame_.FillRect(viewport_.ToPhysical(rect), color, opacity);
    }
    void DrawText(Point position, const char* text, Color color, SystemFont font = SystemFont::kMedium) {
        frame_.DrawText(viewport_.ToPhysical(position), text, color, viewport_.ToPhysical(font));
    }
    void DrawText(Point position, const char* text, Color color, const Font& font) {
        frame_.DrawText(viewport_.ToPhysical(position), text, color, font);
    }
    void DrawTextCentered(int32_t center_x, int32_t y, const char* text, Color color,
                          SystemFont font = SystemFont::kMedium) {
        const Point physical = viewport_.ToPhysical(Point{center_x, y});
        frame_.DrawTextCentered(physical.x, physical.y, text, color, viewport_.ToPhysical(font));
    }
    void DrawTextCentered(int32_t center_x, int32_t y, const char* text, Color color, const Font& font) {
        const Point physical = viewport_.ToPhysical(Point{center_x, y});
        frame_.DrawTextCentered(physical.x, physical.y, text, color, font);
    }

    void DrawTexture(Point position, const Texture& texture, uint8_t opacity = 255U) {
        DrawTexture(position, texture, {0, 0, static_cast<int32_t>(texture.width()),
                                       static_cast<int32_t>(texture.height())},
                    opacity);
    }
    void DrawTexture(Point position, const Texture& texture, Rect source, uint8_t opacity = 255U) {
        const Rect destination{position.x, position.y, source.width, source.height};
        frame_.DrawTexture(viewport_.ToPhysical(destination), texture, source, opacity);
    }
    void DrawTexture(Rect destination, const Texture& texture, uint8_t opacity = 255U) {
        frame_.DrawTexture(viewport_.ToPhysical(destination), texture, opacity);
    }
    void DrawTexture(Rect destination, const Texture& texture, Rect source, uint8_t opacity = 255U) {
        frame_.DrawTexture(viewport_.ToPhysical(destination), texture, source, opacity);
    }
    void DrawTexture(Point position, const StreamingTexture& texture, uint8_t opacity = 255U) {
        DrawTexture(position, texture, {0, 0, static_cast<int32_t>(texture.width()),
                                       static_cast<int32_t>(texture.height())},
                    opacity);
    }
    void DrawTexture(Point position, const StreamingTexture& texture, Rect source, uint8_t opacity = 255U) {
        const Rect destination{position.x, position.y, source.width, source.height};
        frame_.DrawTexture(viewport_.ToPhysical(destination), texture, source, opacity);
    }
    void DrawTexture(Rect destination, const StreamingTexture& texture, uint8_t opacity = 255U) {
        frame_.DrawTexture(viewport_.ToPhysical(destination), texture, opacity);
    }
    void DrawTexture(Rect destination, const StreamingTexture& texture, Rect source, uint8_t opacity = 255U) {
        frame_.DrawTexture(viewport_.ToPhysical(destination), texture, source, opacity);
    }

    void Save() { frame_.Save(); }
    void SetClipRect(Rect clip) { frame_.SetClipRect(viewport_.ToPhysical(clip)); }
    void Translate(Point offset) { frame_.Translate(viewport_.DeltaToPhysical(offset)); }
    void Restore() { frame_.Restore(); }
    [[nodiscard]] Result<void> Present() { return frame_.Present(); }
    [[nodiscard]] constexpr uint32_t draw_operation_count() const { return frame_.draw_operation_count(); }

   private:
    Frame frame_;
    const Viewport& viewport_;
};

}  // namespace micropixel::ui

#endif
