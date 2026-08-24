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
        for (uint32_t index = 0U; index < 3U; ++index) {
            buttons_[index].SetBounds(BottomButtonRect(context, index, 3U));
            buttons_[index].Reset();
        }
        context.app.log().Info("demo.timer: entered; shared 100 ms Timer is running");
    }

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

    void Render(DemoContext& context, micropixel::Frame& commands) {
        const int32_t center_x = static_cast<int32_t>(context.display.width() / 2U);
        commands.DrawTextCentered(center_x, 130, "A Host Timer drives this page every 100 ms.", MutedColor(), 18U);

        Line elapsed;
        elapsed.Append("Elapsed: ");
        elapsed.AppendUint(elapsed_us_ / 1000U);
        elapsed.Append(" ms");
        commands.DrawTextCentered(center_x, 238, elapsed.c_str(), AccentColor(), 36U);

        Line ticks;
        ticks.Append("Timer events: ");
        ticks.AppendUint(tick_count_);
        commands.DrawTextCentered(center_x, 310, ticks.c_str(), micropixel::Color::White(), 22U);

        Line clock;
        clock.Append("Clock::Now(): ");
        clock.AppendUint(context.app.clock().Now().microseconds());
        clock.Append(" us");
        commands.DrawTextCentered(center_x, 366, clock.c_str(), MutedColor(), 18U);
        commands.DrawTextCentered(center_x, 422, running_ ? "RUNNING" : "PAUSED",
                                  running_ ? AccentColor() : DangerColor(), 24U);

        DrawButton(commands, buttons_[0], "START", AccentColor());
        DrawButton(commands, buttons_[1], "PAUSE", DangerColor());
        DrawButton(commands, buttons_[2], "RESET", BlueColor());
    }

   private:
    uint64_t elapsed_us_{};
    uint32_t tick_count_{};
    bool running_{true};
    micropixel::ui::Button buttons_[3]{};
};

TimerPage timer_page;

}  // namespace

micropixel::Timer CreateDemoTicker(micropixel::Application& app) { return app.timers().Every(100_ms); }

void TimerDemoEnter(DemoContext& context) { timer_page.Enter(context); }

bool TimerDemoOnTimer(DemoContext& context, const micropixel::TimerEvent& event) {
    return timer_page.OnTimer(context, event);
}

bool TimerDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event) {
    return timer_page.OnTouch(context, event);
}

void TimerDemoRender(DemoContext& context, micropixel::Frame& commands) { timer_page.Render(context, commands); }

}  // namespace demo
