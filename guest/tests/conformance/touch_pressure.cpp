#include <stdint.h>

#include "sdk/micropixel.hpp"

extern "C" __attribute__((export_name("__micropixel_test_touch_pressure"))) void __micropixel_test_touch_pressure(
    void) {}

int main() {
    micropixel::Application app;
    if (app.input().info().supports_pressure()) {
        return 99;
    }
    bool saw_down = false;
    uint32_t move_count = 0U;

    for (;;) {
        micropixel::Event event = app.WaitEvent();
        const micropixel::TouchEvent* touch = event.touch();
        if (touch == nullptr || touch->id() != 7U || touch->x() < 0 || touch->x() >= 720 || touch->y() < 0 ||
            touch->y() >= 720 || touch->has_pressure() || touch->pressure_per_mille() != 0U) {
            return 100;
        }
        if (touch->phase() == micropixel::TouchPhase::kDown) {
            if (saw_down) {
                return 101;
            }
            saw_down = true;
        } else if (touch->phase() == micropixel::TouchPhase::kMove) {
            if (!saw_down) {
                return 102;
            }
            ++move_count;
        } else if (touch->phase() == micropixel::TouchPhase::kUp) {
            if (!saw_down || move_count == 0U || move_count >= 5000U) {
                return 103;
            }
            app.log().Info("touch_pressure: required Down/Up preserved and Move burst coalesced");
            return 0;
        } else {
            return 104;
        }
    }
}
