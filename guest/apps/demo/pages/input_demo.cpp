// Input and hardware Random usage for the navigable Demo app. See ../README.md.

#include "apps/demo/demo_page.hpp"

namespace demo {

namespace {

struct TouchPoint final {
    bool active{};
    uint32_t id{};
    uint16_t x{};
    uint16_t y{};
    micropixel::Color color{AccentColor()};
};

class InputPage final {
   public:
    void Enter(DemoContext& context) {
        for (TouchPoint& point : points_) {
            point.active = false;
        }
        event_count_ = 0U;
        last_random_ = context.app.random().U32();
        context.app.log().Info("demo.input: touch canvas ready; colors use the hardware RNG");
    }

    [[nodiscard]] bool OnTouch(DemoContext& context, const micropixel::TouchEvent& event) {
        TouchPoint* point = FindActive(event.id());
        if (event.phase() == micropixel::TouchPhase::kDown) {
            if (point == nullptr) {
                point = FindFree();
            }
            if (point == nullptr) {
                return false;
            }
            last_random_ = context.app.random().U32();
            point->color = RandomColor(last_random_);
            point->active = true;
            point->id = event.id();
        } else if (point == nullptr) {
            return false;
        }

        point->x = event.x();
        point->y = event.y();
        if (event.phase() == micropixel::TouchPhase::kUp || event.phase() == micropixel::TouchPhase::kCancel) {
            point->active = false;
        }
        ++event_count_;
        return true;
    }

    void Render(DemoContext& context, micropixel::Frame& commands) {
        const int32_t center_x = static_cast<int32_t>(context.display.width() / 2U);
        commands.DrawTextCentered(center_x, 116, "Touch and drag with one or more fingers.", MutedColor(), 18U);
        commands.FillRect(micropixel::Rect{28, 158, static_cast<int32_t>(context.display.width()) - 56,
                                           static_cast<int32_t>(context.display.height()) - 254},
                          PanelColor());

        for (const TouchPoint& point : points_) {
            if (!point.active) {
                continue;
            }
            int32_t x = static_cast<int32_t>(point.x) - 22;
            int32_t y = static_cast<int32_t>(point.y) - 22;
            const int32_t maximum_x = static_cast<int32_t>(context.display.width()) - 44;
            const int32_t maximum_y = static_cast<int32_t>(context.display.height()) - 44;
            if (x < 0) {
                x = 0;
            } else if (x > maximum_x) {
                x = maximum_x;
            }
            if (y < 100) {
                y = 100;
            } else if (y > maximum_y) {
                y = maximum_y;
            }
            commands.FillRect(micropixel::Rect{x, y, 44, 44}, point.color);
        }

        Line status;
        status.Append("Touch events: ");
        status.AppendUint(event_count_);
        status.Append("   Random::U32(): ");
        status.AppendUint(last_random_);
        commands.DrawTextCentered(center_x, static_cast<int32_t>(context.display.height()) - 60, status.c_str(),
                                  micropixel::Color::White(), 17U);
    }

   private:
    [[nodiscard]] TouchPoint* FindActive(uint32_t id) {
        for (TouchPoint& point : points_) {
            if (point.active && point.id == id) {
                return &point;
            }
        }
        return nullptr;
    }

    [[nodiscard]] TouchPoint* FindFree() {
        for (TouchPoint& point : points_) {
            if (!point.active) {
                return &point;
            }
        }
        return nullptr;
    }

    [[nodiscard]] static micropixel::Color RandomColor(uint32_t value) {
        const uint8_t red = static_cast<uint8_t>(64U + (value & 0x7fU));
        const uint8_t green = static_cast<uint8_t>(64U + ((value >> 8U) & 0x7fU));
        const uint8_t blue = static_cast<uint8_t>(64U + ((value >> 16U) & 0x7fU));
        return micropixel::Color::Rgb(red, green, blue);
    }

    TouchPoint points_[5]{};
    uint32_t event_count_{};
    uint32_t last_random_{};
};

InputPage input_page;

}  // namespace

void InputDemoEnter(DemoContext& context) { input_page.Enter(context); }

bool InputDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event) {
    return input_page.OnTouch(context, event);
}

void InputDemoRender(DemoContext& context, micropixel::Frame& commands) { input_page.Render(context, commands); }

}  // namespace demo
