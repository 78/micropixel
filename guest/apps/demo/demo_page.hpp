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

struct DemoContext final {
    micropixel::Application& app;
    micropixel::Renderer renderer;
    micropixel::InputInfo input;
    DemoLayout layout;
    DemoAtlasTextures& atlas_textures;
    micropixel::AudioClip audio_clip{};
    micropixel::Playback audio_playback{};
};

using Line = micropixel::FixedString<96U>;

[[nodiscard]] inline constexpr int32_t ContentWidth(const DemoContext& context) {
    return context.layout.screen.width;
}

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
        micropixel::ui::FlexLayout{.direction = micropixel::ui::FlexDirection::kHorizontal,
                                   .padding = {vertical_padding, horizontal_padding, vertical_padding,
                                               horizontal_padding},
                                   .gap_pixels = context.layout.compact() ? 10 : 14},
        std::span<const micropixel::ui::FlexItem>{items.data(), buttons.size()},
        std::span<micropixel::Rect>{rects.data(), buttons.size()});
    micropixel::Assert(result.has_value(), "demo: action button layout failed");
    for (uint32_t index = 0U; index < buttons.size(); ++index) {
        buttons[index].SetBounds(rects[index]);
        buttons[index].Reset();
    }
}

inline void DrawButton(micropixel::Frame& commands, const micropixel::ui::Button& button, const char* label,
                       micropixel::Color color = BlueColor()) {
    micropixel::ui::DrawTextButton(commands, button, label,
                                   micropixel::ui::ButtonStyle{.background = color,
                                                               .text = micropixel::Color::White(),
                                                               .feedback_overlay = micropixel::Color::Black(),
                                                               .pressed_opacity = 48U,
                                                               .disabled_opacity = 112U,
                                                               .font = micropixel::SystemFont::kLarge,
                                                               .pressed_text_offset_px = 1});
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
void TimerDemoRender(DemoContext& context, micropixel::Frame& commands);

void InputDemoEnter(DemoContext& context);
[[nodiscard]] bool InputDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void InputDemoRender(DemoContext& context, micropixel::Frame& commands);

void StorageDemoEnter(DemoContext& context);
[[nodiscard]] bool StorageDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void StorageDemoRender(DemoContext& context, micropixel::Frame& commands);

void ResourceAtlasDemoEnter(DemoContext& context);
[[nodiscard]] bool ResourceAtlasDemoOnTimer(DemoContext& context, const micropixel::TimerEvent& event);
[[nodiscard]] bool ResourceAtlasDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void ResourceAtlasDemoRender(DemoContext& context, micropixel::Frame& commands);

void AudioDemoEnter(DemoContext& context);
void AudioDemoExit(DemoContext& context);
[[nodiscard]] bool AudioDemoOnEvent(DemoContext& context, const micropixel::Event& event);
[[nodiscard]] bool AudioDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void AudioDemoRender(DemoContext& context, micropixel::Frame& commands);

void DeviceDemoEnter(DemoContext& context);
void DeviceDemoExit(DemoContext& context);
[[nodiscard]] bool DeviceDemoOnTimer(DemoContext& context, const micropixel::TimerEvent& event);
[[nodiscard]] bool DeviceDemoOnEvent(DemoContext& context, const micropixel::Event& event);
[[nodiscard]] bool DeviceDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void DeviceDemoRender(DemoContext& context, micropixel::Frame& commands);

}  // namespace demo

#endif
