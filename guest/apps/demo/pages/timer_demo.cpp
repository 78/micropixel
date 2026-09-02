// Timer, Clock, and Log usage for the navigable Demo app. See ../README.md.

#include "apps/demo/demo_page.hpp"

using micropixel::literals::operator""_ms;

namespace demo {

namespace {

class TimerPage final {
   public:
    void Enter(DemoContext& context) {
        elapsed_us_ = 0U;
        tick_count_ = 0U;
        running_ = true;
        page_container_ = context.root_container.CreateContainer();
        CreateButtons(context);
        context.app.log().Info("demo.timer: entered; shared 100 ms Timer is running");
    }

    void Exit() { micropixel::Assert(page_container_.Destroy().has_value(), "demo.timer: destroy page failed"); }

    [[nodiscard]] bool OnTimer(DemoContext&, const micropixel::TimerEvent& event) {
        if (!running_) {
            return false;
        }
        elapsed_us_ += event.delta().count_microseconds();
        ++tick_count_;
        return true;
    }

    [[nodiscard]] bool OnTouch(DemoContext& context, const micropixel::TouchEvent& event) {
        bool redraw = false;
        for (uint32_t index = 0U; index < 3U; ++index) {
            const micropixel::ui::ButtonUpdate update = buttons_[index].OnTouch(event);
            redraw = redraw || update.redraw();
            if (!update.clicked) {
                continue;
            }
            if (index == 0U) {
                running_ = true;
                context.app.log().Info("demo.timer: start");
            } else if (index == 1U) {
                running_ = false;
                context.app.log().Info("demo.timer: pause");
            } else {
                elapsed_us_ = 0U;
                tick_count_ = 0U;
                context.app.log().Info("demo.timer: reset");
            }
        }
        return redraw;
    }

    void Render(DemoContext& context, DemoView& commands) {
        const int32_t center_x = PageCenterX(context);
        commands.CenteredText(center_x, PageY(context, 14, 30), "A Host Timer drives this page every 100 ms.",
                              MutedColor(), micropixel::SystemFont::kMedium);

        Line elapsed;
        elapsed.Append("Elapsed: ");
        elapsed.AppendUint(elapsed_us_ / 1000U);
        elapsed.Append(" ms");
        commands.CenteredText(
            center_x, PageY(context, 82, 138), elapsed.c_str(), AccentColor(),
            context.layout.compact() ? micropixel::SystemFont::kLarge : micropixel::SystemFont::kTitle);

        Line ticks;
        ticks.Append("Timer events: ");
        ticks.AppendUint(tick_count_);
        commands.CenteredText(center_x, PageY(context, 138, 210), ticks.c_str(), micropixel::Color::White(),
                              micropixel::SystemFont::kLarge);

        Line clock;
        clock.Append("Clock::Now(): ");
        clock.AppendUint(context.app.clock().Now().microseconds());
        clock.Append(" us");
        commands.CenteredText(center_x, PageY(context, 190, 266), clock.c_str(), MutedColor(),
                              micropixel::SystemFont::kMedium);
        commands.CenteredText(center_x, PageY(context, 240, 322), running_ ? "RUNNING" : "PAUSED",
                              running_ ? AccentColor() : DangerColor(), micropixel::SystemFont::kLarge);

        for (micropixel::ui::TextButton& button : buttons_) {
            button.Sync(commands.scene_update());
        }
    }

   private:
    void CreateButtons(DemoContext& context) {
        std::array<micropixel::Rect, 3U> bounds{};
        LayoutButtonRow(context, bounds);
        constexpr const char* labels[3]{"START", "PAUSE", "RESET"};
        const micropixel::Color backgrounds[3]{AccentColor(), DangerColor(), BlueColor()};
        for (uint32_t index = 0U; index < 3U; ++index) {
            buttons_[index] = page_container_.CreateTextButton(
                {.bounds = bounds[index],
                 .text = labels[index],
                 .style = {.background = backgrounds[index], .font = micropixel::SystemFont::kLarge}});
        }
    }

    uint64_t elapsed_us_{};
    uint32_t tick_count_{};
    bool running_{true};
    micropixel::ContainerNode page_container_{};
    micropixel::ui::TextButton buttons_[3]{};
};

TimerPage timer_page;

}  // namespace

micropixel::Timer CreateDemoTicker(micropixel::Application& app) { return app.timers().Every(100_ms); }

void TimerDemoEnter(DemoContext& context) { timer_page.Enter(context); }

void TimerDemoExit(DemoContext&) { timer_page.Exit(); }

bool TimerDemoOnTimer(DemoContext& context, const micropixel::TimerEvent& event) {
    return timer_page.OnTimer(context, event);
}

bool TimerDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event) {
    return timer_page.OnTouch(context, event);
}

void TimerDemoRender(DemoContext& context, DemoView& commands) { timer_page.Render(context, commands); }

}  // namespace demo
