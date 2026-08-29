#include <stdint.h>

#include <array>
#include <optional>
#include <span>

#include "apps/demo/demo.hpp"
#include "apps/demo/demo_page.hpp"

namespace demo {

namespace {

struct PageEntry final {
    PageId id;
    const char* label;
};

constexpr std::array<PageEntry, 6U> kPages{{
    {PageId::kTimer, "TIMER / CLOCK / LOG"},
    {PageId::kInput, "INPUT / RANDOM"},
    {PageId::kStorage, "STORAGE"},
    {PageId::kResourceAtlas, "RESOURCE / ATLAS"},
    {PageId::kAudio, "AUDIO"},
    {PageId::kDevices, "DEVICES / HARDWARE"},
}};

constexpr uint32_t kPageCount = static_cast<uint32_t>(kPages.size());

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
        case PageId::kDevices:
            return "DEVICES / HARDWARE";
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
        case PageId::kDevices:
            DeviceDemoEnter(context);
            return;
        case PageId::kHome:
            return;
    }
}

void ExitPage(PageId page, DemoContext& context) {
    if (page == PageId::kAudio) {
        AudioDemoExit(context);
    } else if (page == PageId::kDevices) {
        DeviceDemoExit(context);
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
        case PageId::kDevices:
            return DeviceDemoOnTouch(context, event);
        case PageId::kHome:
            return false;
    }
    __builtin_unreachable();
}

void RenderPageContent(PageId page, DemoContext& context, DemoView& view) {
    switch (page) {
        case PageId::kTimer:
            TimerDemoRender(context, view);
            return;
        case PageId::kInput:
            InputDemoRender(context, view);
            return;
        case PageId::kStorage:
            StorageDemoRender(context, view);
            return;
        case PageId::kResourceAtlas:
            ResourceAtlasDemoRender(context, view);
            return;
        case PageId::kAudio:
            AudioDemoRender(context, view);
            return;
        case PageId::kDevices:
            DeviceDemoRender(context, view);
            return;
        case PageId::kHome:
            return;
    }
}

void RenderHome(DemoContext& context, std::span<const micropixel::ui::Button> menu_buttons) {
    context.view.Update([&](DemoView& view) {
        view.CenteredText(context.layout.home_header.center_x(), context.layout.home_header.y + 8,
                          "MICROPIXEL SDK DEMO", micropixel::Color::White(),
                          context.layout.compact() ? micropixel::SystemFont::kLarge : micropixel::SystemFont::kTitle);
        view.CenteredText(context.layout.home_header.center_x(),
                          context.layout.home_header.y + (context.layout.compact() ? 44 : 54),
                          "One app / six focused modules", MutedColor(), micropixel::SystemFont::kMedium);
        for (uint32_t index = 0U; index < menu_buttons.size(); ++index) {
            const micropixel::ui::Button& button = menu_buttons[index];
            const micropixel::Rect bounds = button.bounds();
            view.Panel(bounds, PanelColor());
            view.Panel({bounds.x, bounds.y, context.layout.compact() ? 4 : 6, bounds.height}, AccentColor());
            view.Panel(bounds, micropixel::Color::Black(), button.pressed() ? 48U : 0U);
            view.Text({bounds.x + (context.layout.compact() ? 18 : 24),
                       bounds.y + (bounds.height - 24) / 2 + kButtonTextOpticalOffsetY + (button.pressed() ? 1 : 0)},
                      kPages[index].label, micropixel::Color::White(),
                      context.layout.compact() ? micropixel::SystemFont::kMedium : micropixel::SystemFont::kLarge);
        }
    });
}

void RenderPage(DemoContext& context, PageId page, const micropixel::ui::Button& back_button) {
    context.view.Update([&](DemoView& view) {
        DrawButton(view, back_button, "BACK", PanelColor());
        const int32_t title_left =
            back_button.bounds().x + back_button.bounds().width + (context.layout.compact() ? 12 : 20);
        const int32_t title_right =
            context.layout.page_header.x + context.layout.page_header.width - (context.layout.compact() ? 16 : 24);
        micropixel::Assert(title_right > title_left, "demo: page title area is empty");
        view.CenteredText(title_left + (title_right - title_left) / 2,
                          context.layout.page_header.y + (context.layout.compact() ? 25 : 31), PageTitle(page),
                          micropixel::Color::White(),
                          context.layout.compact() ? micropixel::SystemFont::kLarge : micropixel::SystemFont::kTitle);
        view.Panel(context.layout.separator, AccentColor());
        RenderPageContent(page, context, view);
    });
}

}  // namespace

DemoLayout BuildDemoLayout(micropixel::RendererInfo display) {
    DemoLayout result{};
    result.display_class =
        display.width() < 600U || display.height() < 600U ? DisplayClass::kCompact : DisplayClass::kExpanded;
    result.screen = {0, 0, static_cast<int32_t>(display.width()), static_cast<int32_t>(display.height())};
    const bool compact = result.compact();

    const std::array home_items{micropixel::ui::FlexItem::Fixed(compact ? 92U : 112U),
                                micropixel::ui::FlexItem::Grow()};
    std::array<micropixel::Rect, home_items.size()> home_rects{};
    auto home = micropixel::ui::ComputeFlexLayout(
        result.screen,
        micropixel::ui::FlexLayout{
            .direction = micropixel::ui::FlexDirection::kVertical,
            .padding = {compact ? 12 : 20, compact ? 20 : 28, compact ? 16 : 24, compact ? 20 : 28},
            .gap_pixels = compact ? 8 : 16},
        home_items, home_rects);
    micropixel::Assert(home.has_value(), "demo: home layout failed");
    result.home_header = home_rects[0];
    result.home_content = home_rects[1];

    if (compact) {
        std::array<micropixel::ui::FlexItem, kPageCount> items{};
        for (micropixel::ui::FlexItem& item : items) {
            item = micropixel::ui::FlexItem::Grow();
        }
        auto menu = micropixel::ui::ComputeFlexLayout(
            result.home_content,
            micropixel::ui::FlexLayout{.direction = micropixel::ui::FlexDirection::kVertical, .gap_pixels = 8}, items,
            result.menu_buttons);
        micropixel::Assert(menu.has_value(), "demo: compact menu layout failed");
    } else {
        constexpr std::array row_items{micropixel::ui::FlexItem::Grow(), micropixel::ui::FlexItem::Grow(),
                                       micropixel::ui::FlexItem::Grow()};
        std::array<micropixel::Rect, row_items.size()> rows{};
        auto menu_rows = micropixel::ui::ComputeFlexLayout(
            result.home_content,
            micropixel::ui::FlexLayout{.direction = micropixel::ui::FlexDirection::kVertical, .gap_pixels = 16},
            row_items, rows);
        micropixel::Assert(menu_rows.has_value(), "demo: expanded menu row layout failed");
        constexpr std::array column_items{micropixel::ui::FlexItem::Grow(), micropixel::ui::FlexItem::Grow()};
        for (uint32_t row = 0U; row < rows.size(); ++row) {
            std::span<micropixel::Rect> output{result.menu_buttons.data() + row * 2U, 2U};
            auto menu_columns = micropixel::ui::ComputeFlexLayout(
                rows[row],
                micropixel::ui::FlexLayout{.direction = micropixel::ui::FlexDirection::kHorizontal, .gap_pixels = 16},
                column_items, output);
            micropixel::Assert(menu_columns.has_value(), "demo: expanded menu column layout failed");
        }
    }

    const std::array page_items{micropixel::ui::FlexItem::Fixed(compact ? 82U : 100U), micropixel::ui::FlexItem::Grow(),
                                micropixel::ui::FlexItem::Fixed(compact ? 76U : 104U)};
    std::array<micropixel::Rect, page_items.size()> page_rects{};
    auto page = micropixel::ui::ComputeFlexLayout(
        result.screen, micropixel::ui::FlexLayout{.direction = micropixel::ui::FlexDirection::kVertical}, page_items,
        page_rects);
    micropixel::Assert(page.has_value(), "demo: page layout failed");
    result.page_header = page_rects[0];
    result.page_content = page_rects[1];
    result.page_actions = page_rects[2];
    result.back_button = compact ? micropixel::Rect{16, 12, 112, 60} : micropixel::Rect{24, 18, 144, 72};
    result.separator = {compact ? 16 : 24, result.page_header.y + result.page_header.height - 2,
                        result.screen.width - (compact ? 32 : 48), 2};
    return result;
}

int DemoAppMain() {
    micropixel::Application app;
    micropixel::Renderer renderer = app.renderer();
    micropixel::RendererInfo display = renderer.info();
    micropixel::InputInfo input = app.input().info();
    micropixel::Assert(display.width() >= 320U && display.height() >= 480U, "demo: display must be at least 320x480");

    DemoAtlasTextures atlas_textures = LoadDemoAtlases(app);
    DemoLayout layout = BuildDemoLayout(display);
    micropixel::Scene scene = renderer.CreateScene(
        {.logical_width = display.width(), .logical_height = display.height(), .background = BackgroundColor()});
    micropixel::Layer layer = scene.CreateLayer({.clip = layout.screen});
    DemoView view(scene, layer, atlas_textures);
    DemoContext context{app, renderer, input, layout, atlas_textures, view};
    micropixel::Timer ticker = CreateDemoTicker(app);
    micropixel::Timer atlas_ticker = CreateResourceAtlasTicker(app);
    std::optional<micropixel::Timer> device_ticker{};
    PageId active_page = PageId::kHome;
    std::array<micropixel::ui::Button, kPageCount> menu_buttons{};
    for (uint32_t index = 0U; index < kPageCount; ++index) {
        menu_buttons[index].SetBounds(context.layout.menu_buttons[index]);
    }
    micropixel::ui::Button back_button{context.layout.back_button, 6U};
    RenderHome(context, menu_buttons);
    app.log().Info("demo: ready; select a module on screen");

    app.Run([&](const micropixel::Event& event) {
        if (event.type() == micropixel::EventType::kStop) {
            ExitPage(active_page, context);
            device_ticker.reset();
            return;
        }
        bool redraw = false;
        if (const micropixel::TimerEvent* timer = event.TimerFrom(ticker)) {
            if (active_page == PageId::kTimer) {
                redraw = TimerDemoOnTimer(context, *timer);
            }
        } else if (const micropixel::TimerEvent* timer = event.TimerFrom(atlas_ticker)) {
            redraw = active_page == PageId::kResourceAtlas && ResourceAtlasDemoOnTimer(context, *timer);
        } else if (const micropixel::TimerEvent* timer =
                       device_ticker.has_value() ? event.TimerFrom(*device_ticker) : nullptr) {
            redraw = active_page == PageId::kDevices && DeviceDemoOnTimer(context, *timer);
        } else if (active_page == PageId::kAudio && event.type() == micropixel::EventType::kAudioPlayback) {
            redraw = AudioDemoOnEvent(context, event);
        } else if (active_page == PageId::kDevices && (event.type() == micropixel::EventType::kGpioEdge ||
                                                       event.type() == micropixel::EventType::kHapticFinished ||
                                                       event.type() == micropixel::EventType::kDeviceAdded ||
                                                       event.type() == micropixel::EventType::kDeviceRemoved)) {
            redraw = DeviceDemoOnEvent(context, event);
        } else if (const micropixel::KeyEvent* key = event.key()) {
            redraw = active_page == PageId::kInput && InputDemoOnKey(context, *key);
        } else if (const micropixel::TouchEvent* touch = event.touch()) {
            if (active_page != PageId::kHome) {
                const micropixel::ui::ButtonUpdate back_update = back_button.OnTouch(*touch);
                redraw = back_update.redraw();
                if (back_update.clicked) {
                    ExitPage(active_page, context);
                    device_ticker.reset();
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
                        if (active_page == PageId::kDevices) {
                            device_ticker.emplace(CreateDeviceTicker(app));
                        }
                        redraw = true;
                        break;
                    }
                }
            }
        } else if (event.type() == micropixel::EventType::kResume) {
            redraw = true;
        }

        if (redraw) {
            if (active_page == PageId::kHome) {
                RenderHome(context, menu_buttons);
            } else {
                RenderPage(context, active_page, back_button);
            }
        }
    });
    return 0;
}

}  // namespace demo
