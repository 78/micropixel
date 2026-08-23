#include <stdint.h>

#include "apps/demo/demo.hpp"
#include "apps/demo/demo_page.hpp"

namespace demo {

namespace {

struct PageEntry final {
    PageId id;
    const char* label;
};

constexpr uint32_t kPageCount = 5U;

const PageEntry kPages[kPageCount] = {
    {PageId::kTimer, "TIMER / CLOCK / LOG"},      {PageId::kInput, "INPUT / RANDOM"}, {PageId::kStorage, "STORAGE"},
    {PageId::kResourceAtlas, "RESOURCE / ATLAS"}, {PageId::kAudio, "AUDIO"},
};

[[nodiscard]] const char* PageTitle(PageId page) {
    switch (page) {
        case PageId::kTimer:
            return "TIMER / CLOCK / LOG";
        case PageId::kInput:
            return "INPUT / RANDOM";
        case PageId::kStorage:
            return "STORAGE";
        case PageId::kResourceAtlas:
            return "RESOURCE / ATLAS";
        case PageId::kAudio:
            return "AUDIO";
        case PageId::kHome:
            return "MICROPIXEL SDK DEMO";
    }
    __builtin_unreachable();
}

void EnterPage(PageId page, DemoContext& context) {
    switch (page) {
        case PageId::kTimer:
            TimerDemoEnter(context);
            return;
        case PageId::kInput:
            InputDemoEnter(context);
            return;
        case PageId::kStorage:
            StorageDemoEnter(context);
            return;
        case PageId::kResourceAtlas:
            ResourceAtlasDemoEnter(context);
            return;
        case PageId::kAudio:
            AudioDemoEnter(context);
            return;
        case PageId::kHome:
            return;
    }
}

void ExitPage(PageId page, DemoContext& context) {
    if (page == PageId::kAudio) {
        AudioDemoExit(context);
    }
}

[[nodiscard]] bool HandleTimer(PageId page, DemoContext& context, const micropixel::TimerEvent& event) {
    switch (page) {
        case PageId::kTimer:
            return TimerDemoOnTimer(context, event);
        case PageId::kResourceAtlas:
            return ResourceAtlasDemoOnTimer(context, event);
        default:
            return false;
    }
}

[[nodiscard]] bool HandleTouch(PageId page, DemoContext& context, const micropixel::TouchEvent& event) {
    switch (page) {
        case PageId::kTimer:
            return TimerDemoOnTouch(context, event);
        case PageId::kInput:
            return InputDemoOnTouch(context, event);
        case PageId::kStorage:
            return StorageDemoOnTouch(context, event);
        case PageId::kResourceAtlas:
            return ResourceAtlasDemoOnTouch(context, event);
        case PageId::kAudio:
            return AudioDemoOnTouch(context, event);
        case PageId::kHome:
            return false;
    }
    __builtin_unreachable();
}

void RenderPageContent(PageId page, DemoContext& context, micropixel::CommandBuffer& commands) {
    switch (page) {
        case PageId::kTimer:
            TimerDemoRender(context, commands);
            return;
        case PageId::kInput:
            InputDemoRender(context, commands);
            return;
        case PageId::kStorage:
            StorageDemoRender(context, commands);
            return;
        case PageId::kResourceAtlas:
            ResourceAtlasDemoRender(context, commands);
            return;
        case PageId::kAudio:
            AudioDemoRender(context, commands);
            return;
        case PageId::kHome:
            return;
    }
}

[[nodiscard]] micropixel::Rect MenuButtonRect(const DemoContext& context, uint32_t index) {
    constexpr int32_t kMargin = 42;
    constexpr int32_t kTop = 132;
    constexpr int32_t kBottom = 34;
    constexpr int32_t kGap = 12;
    const int32_t display_width = static_cast<int32_t>(context.display.width());
    const int32_t display_height = static_cast<int32_t>(context.display.height());
    const int32_t available = display_height - kTop - kBottom - kGap * static_cast<int32_t>(kPageCount - 1U);
    const int32_t height = available / static_cast<int32_t>(kPageCount);
    return micropixel::Rect{kMargin, kTop + static_cast<int32_t>(index) * (height + kGap), display_width - kMargin * 2,
                            height};
}

void RenderHome(DemoContext& context, const micropixel::ui::Button (&menu_buttons)[kPageCount]) {
    micropixel::CommandBuffer commands = context.graphics.CreateCommandBuffer(context.display);
    commands.Clear(BackgroundColor());
    commands.DrawTextCentered(static_cast<int32_t>(context.display.width() / 2U), 34, "MICROPIXEL SDK DEMO",
                              micropixel::Color::White(), 32U);
    commands.DrawTextCentered(static_cast<int32_t>(context.display.width() / 2U), 82, "One app / five focused modules",
                              MutedColor(), 18U);
    for (uint32_t index = 0U; index < kPageCount; ++index) {
        const micropixel::ui::Button& button = menu_buttons[index];
        const micropixel::Rect bounds = button.bounds();
        commands.FillRect(bounds, PanelColor());
        commands.FillRect(micropixel::Rect{bounds.x, bounds.y, 6, bounds.height}, AccentColor());
        commands.BlendRect(bounds, micropixel::Color::Black(), button.pressed() ? 48U : 0U);
        commands.DrawText(bounds.x + 30, bounds.y + (bounds.height - 24) / 2 + (button.pressed() ? 1 : 0),
                          kPages[index].label, micropixel::Color::White(), 22U);
    }
    commands.Submit();
}

void RenderPage(DemoContext& context, PageId page, const micropixel::ui::Button& back_button) {
    micropixel::CommandBuffer commands = context.graphics.CreateCommandBuffer(context.display);
    commands.Clear(BackgroundColor());
    DrawButton(commands, back_button, "BACK", PanelColor());
    commands.DrawTextCentered(static_cast<int32_t>(context.display.width() / 2U), 31, PageTitle(page),
                              micropixel::Color::White(), 28U);
    commands.FillRect(micropixel::Rect{24, 92, static_cast<int32_t>(context.display.width()) - 48, 2}, AccentColor());
    RenderPageContent(page, context, commands);
    commands.Submit();
}

}  // namespace

int DemoAppMain() {
    micropixel::Application app;
    micropixel::Graphics graphics = app.graphics();
    micropixel::GraphicsInfo display = graphics.info();
    micropixel::InputInfo input = app.input().info();
    micropixel::AssertThat(display.width() >= 320U && display.height() >= 480U,
                           "demo: display must be at least 320x480");
    micropixel::AssertThat(input.logical_width() == display.width() && input.logical_height() == display.height(),
                           "demo: input and display coordinates must match");

    micropixel::Bitmap atlas_bitmap = LoadDemoAtlas(app);
    DemoContext context{app, graphics, display, input, atlas_bitmap};
    micropixel::Timer ticker = CreateDemoTicker(app);
    PageId active_page = PageId::kHome;
    micropixel::ui::Button menu_buttons[kPageCount]{};
    for (uint32_t index = 0U; index < kPageCount; ++index) {
        menu_buttons[index].SetBounds(MenuButtonRect(context, index));
    }
    micropixel::ui::Button back_button{BackButtonRect()};
    RenderHome(context, menu_buttons);
    app.log().Info("demo: ready; select a module on screen");

    for (;;) {
        micropixel::Event event = app.WaitEvent();
        bool redraw = false;
        if (const micropixel::TimerEvent* timer = event.TimerFrom(ticker)) {
            redraw = HandleTimer(active_page, context, *timer);
        } else if (const micropixel::TouchEvent* touch = event.touch()) {
            if (active_page != PageId::kHome) {
                const micropixel::ui::ButtonUpdate back_update = back_button.OnTouch(*touch);
                redraw = back_update.redraw();
                if (back_update.clicked) {
                    ExitPage(active_page, context);
                    active_page = PageId::kHome;
                    back_button.Reset();
                    for (micropixel::ui::Button& button : menu_buttons) {
                        button.Reset();
                    }
                    redraw = true;
                } else if (!back_update.handled) {
                    redraw = HandleTouch(active_page, context, *touch) || redraw;
                }
            } else {
                for (uint32_t index = 0U; index < kPageCount; ++index) {
                    const micropixel::ui::ButtonUpdate update = menu_buttons[index].OnTouch(*touch);
                    redraw = update.redraw() || redraw;
                    if (update.clicked) {
                        active_page = kPages[index].id;
                        for (micropixel::ui::Button& button : menu_buttons) {
                            button.Reset();
                        }
                        back_button.Reset();
                        EnterPage(active_page, context);
                        redraw = true;
                        break;
                    }
                }
            }
        }

        if (redraw) {
            if (active_page == PageId::kHome) {
                RenderHome(context, menu_buttons);
            } else {
                RenderPage(context, active_page, back_button);
            }
        }
    }
}

}  // namespace demo
