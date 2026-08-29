#ifndef MICROPIXEL_DEMO_PAGE_HPP
#define MICROPIXEL_DEMO_PAGE_HPP

#include <stdint.h>

#include <array>
#include <span>

#include "sdk/micropixel.hpp"

namespace demo {

inline constexpr micropixel::Color BackgroundColor() { return micropixel::Color::Rgb(7U, 14U, 27U); }
inline constexpr micropixel::Color PanelColor() { return micropixel::Color::Rgb(17U, 32U, 52U); }
inline constexpr micropixel::Color AccentColor() { return micropixel::Color::Rgb(67U, 214U, 166U); }
inline constexpr micropixel::Color MutedColor() { return micropixel::Color::Rgb(148U, 168U, 190U); }
inline constexpr micropixel::Color DangerColor() { return micropixel::Color::Rgb(224U, 79U, 96U); }
inline constexpr micropixel::Color BlueColor() { return micropixel::Color::Rgb(68U, 132U, 235U); }
inline constexpr int32_t kButtonTextOpticalOffsetY = -5;

enum class DisplayClass : uint8_t {
    kCompact,
    kExpanded,
};

struct DemoLayout final {
    DisplayClass display_class{};
    micropixel::Rect screen{};
    micropixel::Rect home_header{};
    micropixel::Rect home_content{};
    micropixel::Rect page_header{};
    micropixel::Rect page_content{};
    micropixel::Rect page_actions{};
    micropixel::Rect back_button{};
    micropixel::Rect separator{};
    std::array<micropixel::Rect, 6U> menu_buttons{};

    [[nodiscard]] constexpr bool compact() const { return display_class == DisplayClass::kCompact; }
};

[[nodiscard]] DemoLayout BuildDemoLayout(micropixel::RendererInfo display);

inline constexpr uint32_t kDemoAtlasSheetCount = 3U;
using DemoAtlasTextures = std::array<micropixel::Texture, kDemoAtlasSheetCount>;

class DemoView final {
   public:
    DemoView(micropixel::Scene& scene, micropixel::Layer layer, DemoAtlasTextures& atlases)
        : scene_(scene), layer_(layer) {
        shapes_ = scene_.CreateSpriteBatch(kShapeCapacity, layer_);
        for (micropixel::LabelNode& label : labels_) {
            label =
                scene_.CreateLabel({0, 0}, " ", micropixel::Color::White(), micropixel::SystemFont::kMedium, layer_);
        }
        for (micropixel::SpriteNode& sprite : sprites_) {
            sprite = scene_.CreateSprite(atlases[0], {0, 0, 1, 1}, {0, 0, 1, 1}, layer_);
        }
    }

    template <typename RenderFunction>
    void Update(RenderFunction&& render) {
        auto update = scene_.BeginUpdate();
        update_ = &update;
        shape_count_ = 0U;
        label_count_ = 0U;
        sprite_count_ = 0U;
        for (uint16_t index = 0U; index < kShapeCapacity; ++index) {
            shapes_.SetInstanceVisible(update, index, false);
        }
        for (micropixel::LabelNode& label : labels_) {
            label.SetVisible(update, false);
        }
        for (micropixel::SpriteNode& sprite : sprites_) {
            sprite.SetVisible(update, false);
        }
        render(*this);
        update_ = nullptr;
        micropixel::Assert(update.Present().has_value(), "demo: scene update failed");
    }

    void Panel(micropixel::Rect rect, micropixel::Color color, uint8_t opacity = 255U) {
        micropixel::Assert(update_ != nullptr && shape_count_ < kShapeCapacity, "demo: shape slots exhausted");
        shapes_.SetInstance(*update_, shape_count_++,
                            {.destination = rect, .color = color, .opacity = opacity, .visible = opacity != 0U});
    }

    void Text(micropixel::Point position, const char* text, micropixel::Color color,
              micropixel::SystemFont font = micropixel::SystemFont::kMedium) {
        SetText(position, text, color, font, false);
    }

    void CenteredText(int32_t center_x, int32_t y, const char* text, micropixel::Color color,
                      micropixel::SystemFont font = micropixel::SystemFont::kMedium) {
        SetText({center_x, y}, text, color, font, true);
    }

    void Sprite(micropixel::Point position, const micropixel::Texture& texture, micropixel::Rect source,
                uint8_t opacity = 255U) {
        micropixel::Assert(update_ != nullptr && sprite_count_ < kSpriteCapacity, "demo: sprite slots exhausted");
        micropixel::SpriteNode& sprite = sprites_[sprite_count_++];
        sprite.SetTexture(*update_, texture);
        sprite.SetSource(*update_, source);
        sprite.SetDestination(*update_, {position.x, position.y, source.width, source.height});
        sprite.SetOpacity(*update_, opacity);
        sprite.SetVisible(*update_, opacity != 0U);
    }

   private:
    void SetText(micropixel::Point position, const char* text, micropixel::Color color, micropixel::SystemFont font,
                 bool centered) {
        micropixel::Assert(update_ != nullptr && label_count_ < kLabelCapacity, "demo: label slots exhausted");
        micropixel::LabelNode& label = labels_[label_count_++];
        label.SetPosition(*update_, position);
        label.SetText(*update_, text);
        label.SetColor(*update_, color);
        label.SetFont(*update_, font);
        label.SetCentered(*update_, centered);
        label.SetVisible(*update_, true);
    }

    static constexpr uint16_t kShapeCapacity = 64U;
    static constexpr uint16_t kLabelCapacity = 64U;
    static constexpr uint16_t kSpriteCapacity = 8U;
    micropixel::Scene& scene_;
    micropixel::Layer layer_{};
    micropixel::SpriteBatch shapes_{};
    micropixel::LabelNode labels_[kLabelCapacity]{};
    micropixel::SpriteNode sprites_[kSpriteCapacity]{};
    micropixel::SceneUpdate* update_{};
    uint16_t shape_count_{};
    uint16_t label_count_{};
    uint16_t sprite_count_{};
};

struct DemoContext final {
    micropixel::Application& app;
    micropixel::Renderer renderer;
    micropixel::InputInfo input;
    DemoLayout layout;
    DemoAtlasTextures& atlas_textures;
    DemoView& view;
    micropixel::AudioClip audio_clip{};
    micropixel::Playback audio_playback{};
};

using Line = micropixel::FixedString<96U>;

[[nodiscard]] inline constexpr int32_t ContentWidth(const DemoContext& context) { return context.layout.screen.width; }

[[nodiscard]] inline constexpr int32_t ContentHeight(const DemoContext& context) {
    return context.layout.screen.height;
}

[[nodiscard]] inline constexpr int32_t PageCenterX(const DemoContext& context) {
    return context.layout.page_content.center_x();
}

[[nodiscard]] inline constexpr int32_t PageY(const DemoContext& context, int32_t compact_offset,
                                             int32_t expanded_offset) {
    return context.layout.page_content.y + (context.layout.compact() ? compact_offset : expanded_offset);
}

inline void LayoutButtonRow(const DemoContext& context, std::span<micropixel::ui::Button> buttons) {
    micropixel::Assert(!buttons.empty() && buttons.size() <= 6U, "demo: invalid action button count");
    std::array<micropixel::ui::FlexItem, 6U> items{};
    std::array<micropixel::Rect, 6U> rects{};
    for (uint32_t index = 0U; index < buttons.size(); ++index) {
        items[index] = micropixel::ui::FlexItem::Grow();
    }
    const int32_t horizontal_padding = context.layout.compact() ? 20 : 28;
    const int32_t vertical_padding = context.layout.compact() ? 12 : 16;
    auto result = micropixel::ui::ComputeFlexLayout(
        context.layout.page_actions,
        micropixel::ui::FlexLayout{
            .direction = micropixel::ui::FlexDirection::kHorizontal,
            .padding = {vertical_padding, horizontal_padding, vertical_padding, horizontal_padding},
            .gap_pixels = context.layout.compact() ? 10 : 14},
        std::span<const micropixel::ui::FlexItem>{items.data(), buttons.size()},
        std::span<micropixel::Rect>{rects.data(), buttons.size()});
    micropixel::Assert(result.has_value(), "demo: action button layout failed");
    for (uint32_t index = 0U; index < buttons.size(); ++index) {
        buttons[index].SetBounds(rects[index]);
        buttons[index].Reset();
    }
}

inline void DrawButton(DemoView& view, const micropixel::ui::Button& button, const char* label,
                       micropixel::Color color = BlueColor()) {
    const micropixel::Rect bounds = button.bounds();
    view.Panel(bounds, color);
    view.Panel(bounds, micropixel::Color::Black(), !button.enabled() ? 112U : button.pressed() ? 48U : 0U);
    view.CenteredText(bounds.center_x(),
                      bounds.y + (bounds.height - 24) / 2 + kButtonTextOpticalOffsetY + (button.pressed() ? 1 : 0),
                      label, micropixel::Color::White(), micropixel::SystemFont::kLarge);
}

enum class PageId : uint8_t {
    kHome,
    kTimer,
    kInput,
    kStorage,
    kResourceAtlas,
    kAudio,
    kDevices,
};

[[nodiscard]] micropixel::Timer CreateDemoTicker(micropixel::Application& app);
[[nodiscard]] micropixel::Timer CreateResourceAtlasTicker(micropixel::Application& app);
[[nodiscard]] micropixel::Timer CreateDeviceTicker(micropixel::Application& app);
[[nodiscard]] DemoAtlasTextures LoadDemoAtlases(micropixel::Application& app);

void TimerDemoEnter(DemoContext& context);
[[nodiscard]] bool TimerDemoOnTimer(DemoContext& context, const micropixel::TimerEvent& event);
[[nodiscard]] bool TimerDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void TimerDemoRender(DemoContext& context, DemoView& view);

void InputDemoEnter(DemoContext& context);
[[nodiscard]] bool InputDemoOnKey(DemoContext& context, const micropixel::KeyEvent& event);
[[nodiscard]] bool InputDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void InputDemoRender(DemoContext& context, DemoView& view);

void StorageDemoEnter(DemoContext& context);
[[nodiscard]] bool StorageDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void StorageDemoRender(DemoContext& context, DemoView& view);

void ResourceAtlasDemoEnter(DemoContext& context);
[[nodiscard]] bool ResourceAtlasDemoOnTimer(DemoContext& context, const micropixel::TimerEvent& event);
[[nodiscard]] bool ResourceAtlasDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void ResourceAtlasDemoRender(DemoContext& context, DemoView& view);

void AudioDemoEnter(DemoContext& context);
void AudioDemoExit(DemoContext& context);
[[nodiscard]] bool AudioDemoOnEvent(DemoContext& context, const micropixel::Event& event);
[[nodiscard]] bool AudioDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void AudioDemoRender(DemoContext& context, DemoView& view);

void DeviceDemoEnter(DemoContext& context);
void DeviceDemoExit(DemoContext& context);
[[nodiscard]] bool DeviceDemoOnTimer(DemoContext& context, const micropixel::TimerEvent& event);
[[nodiscard]] bool DeviceDemoOnEvent(DemoContext& context, const micropixel::Event& event);
[[nodiscard]] bool DeviceDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void DeviceDemoRender(DemoContext& context, DemoView& view);

}  // namespace demo

#endif
