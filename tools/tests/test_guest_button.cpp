#include <cstdlib>
#include <cstring>
#include <iostream>

#include "sdk/ui/button.hpp"

namespace micropixel {

class Application final {
   public:
    [[nodiscard]] static constexpr TouchEvent Touch(TouchPhase phase, uint32_t id, int32_t x, int32_t y,
                                                    uint64_t timestamp_us = 0U) {
        return TouchEvent{TimePoint{timestamp_us}, phase, id, x, y, false, 0U};
    }
};

}  // namespace micropixel

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void ExpandedHitAreaCapturesWithoutChangingVisualBounds() {
    micropixel::ui::Button button{{100, 100, 32, 32}, 6U};
    Check(button.bounds() == micropixel::Rect{100, 100, 32, 32}, "visual bounds must remain unchanged");
    Check(button.hit_bounds() == micropixel::Rect{94, 94, 44, 44}, "hit padding must expand all four edges");

    const auto down = button.OnTouch(
        micropixel::Application::Touch(micropixel::TouchPhase::kDown, 7U, 95, 110, 100U));
    Check(down.handled && down.visual_changed && !down.clicked && button.pressed(),
          "a down inside only the expanded area must capture and press the button");
    const auto up =
        button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kUp, 7U, 95, 110, 200U));
    Check(up.handled && up.clicked && !button.tracking(), "release inside the expanded area must click once");
}

void BackgroundTouchCannotRetargetOnRelease() {
    micropixel::ui::Button button{{100, 100, 32, 32}, 6U};
    const auto down = button.OnTouch(
        micropixel::Application::Touch(micropixel::TouchPhase::kDown, 3U, 93, 110, 100U));
    Check(!down.handled && !button.tracking(), "a down outside the expanded area must remain unclaimed");
    const auto up =
        button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kUp, 3U, 110, 110, 200U));
    Check(!up.handled && !up.clicked, "an unclaimed background touch must not retarget to the button on release");
}

void CapturedTouchCanLeaveAndReturn() {
    micropixel::ui::Button button{{100, 100, 32, 32}, 6U};
    (void)button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kDown, 11U, 95, 110));
    const auto leave =
        button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kMove, 11U, 90, 110));
    Check(leave.handled && leave.visual_changed && !button.pressed(),
          "leaving the expanded area must clear pressed feedback");
    const auto enter =
        button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kMove, 11U, 95, 110));
    Check(enter.handled && enter.visual_changed && button.pressed(),
          "returning to the expanded area must restore pressed feedback");
    const auto up = button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kUp, 11U, 95, 110));
    Check(up.clicked, "a captured touch returning before release must click");
}

void DescribesInteractionState() {
    micropixel::ui::Button button{{100, 100, 32, 32}, 6U};
    (void)button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kDown, 11U, 95, 110));
    const auto description = button.ToString();
    Check(std::strcmp(description.c_str(),
                      "Button bounds=(x=100,y=100,w=32,h=32) hit_bounds=(x=94,y=94,w=44,h=44) "
                      "enabled=true tracking=true pressed=true touch_id=11") == 0,
          "Button::ToString must include geometry and captured touch state");
}

}  // namespace

int main() {
    ExpandedHitAreaCapturesWithoutChangingVisualBounds();
    BackgroundTouchCannotRetargetOnRelease();
    CapturedTouchCanLeaveAndReturn();
    DescribesInteractionState();
    std::cout << "guest button tests passed: 4 cases\n";
    return 0;
}
