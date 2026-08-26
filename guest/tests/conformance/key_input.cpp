#include <stdint.h>

#include "sdk/micropixel.hpp"

extern "C" __attribute__((export_name("__micropixel_test_key_input"))) void __micropixel_test_key_input(void) {}

int main() {
    static_assert(static_cast<uint16_t>(micropixel::KeyCode::kSouth) == 8U);
    static_assert(static_cast<uint16_t>(micropixel::KeyCode::kEast) == 9U);
    static_assert(static_cast<uint16_t>(micropixel::KeyCode::kWest) == 10U);
    static_assert(static_cast<uint16_t>(micropixel::KeyCode::kNorth) == 11U);

    micropixel::Application app;
    if (!app.input().info().supports_key_events()) {
        return 110;
    }

    constexpr micropixel::KeyPhase kExpectedPhases[] = {
        micropixel::KeyPhase::kDown,
        micropixel::KeyPhase::kRepeat,
        micropixel::KeyPhase::kUp,
    };
    for (uint32_t index = 0U; index < 3U; ++index) {
        const micropixel::Event event = app.WaitEvent();
        const micropixel::KeyEvent* key = event.key();
        if (key == nullptr || key->code() != micropixel::KeyCode::kConfirm || key->phase() != kExpectedPhases[index]) {
            return 111;
        }
        const uint32_t expected_repeat_count = index == 1U ? 2U : 0U;
        if (key->repeat_count() != expected_repeat_count) {
            return 112;
        }
    }
    app.log().Info("key_input: Down/Repeat/Up preserved");
    return 0;
}
