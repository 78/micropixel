#ifndef MICROPIXEL_DEMO_PAGE_HPP
#define MICROPIXEL_DEMO_PAGE_HPP

#include <stdint.h>

#include "sdk/micropixel.hpp"

namespace demo {

inline constexpr micropixel::Color BackgroundColor() { return micropixel::Color::Rgb(7U, 14U, 27U); }
inline constexpr micropixel::Color PanelColor() { return micropixel::Color::Rgb(17U, 32U, 52U); }
inline constexpr micropixel::Color AccentColor() { return micropixel::Color::Rgb(67U, 214U, 166U); }
inline constexpr micropixel::Color MutedColor() { return micropixel::Color::Rgb(148U, 168U, 190U); }
inline constexpr micropixel::Color DangerColor() { return micropixel::Color::Rgb(224U, 79U, 96U); }
inline constexpr micropixel::Color BlueColor() { return micropixel::Color::Rgb(68U, 132U, 235U); }

struct DemoContext final {
    micropixel::Application& app;
    micropixel::Renderer renderer;
    micropixel::RendererInfo display;
    micropixel::InputInfo input;
    micropixel::Texture& atlas_texture;
};

using Line = micropixel::FixedString<96U>;

[[nodiscard]] inline micropixel::Rect BackButtonRect() { return micropixel::Rect{24, 20, 112, 56}; }

[[nodiscard]] inline micropixel::Rect BottomButtonRect(const DemoContext& context, uint32_t index, uint32_t count) {
    constexpr int32_t kMargin = 28;
    constexpr int32_t kGap = 14;
    constexpr int32_t kHeight = 72;
    const int32_t display_width = static_cast<int32_t>(context.display.width());
    const int32_t display_height = static_cast<int32_t>(context.display.height());
    const int32_t total_gaps = static_cast<int32_t>(count > 0U ? count - 1U : 0U) * kGap;
    const int32_t width = (display_width - kMargin * 2 - total_gaps) / static_cast<int32_t>(count);
    return micropixel::Rect{kMargin + static_cast<int32_t>(index) * (width + kGap), display_height - 96, width,
                            kHeight};
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
};

[[nodiscard]] micropixel::Timer CreateDemoTicker(micropixel::Application& app);
[[nodiscard]] micropixel::Timer CreateResourceAtlasTicker(micropixel::Application& app);
[[nodiscard]] micropixel::Texture LoadDemoAtlas(micropixel::Application& app);

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
[[nodiscard]] bool AudioDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void AudioDemoRender(DemoContext& context, micropixel::Frame& commands);

}  // namespace demo

#endif
