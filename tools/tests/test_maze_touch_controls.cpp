// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <initializer_list>

#include "apps/maze-evil/game/run_record.hpp"
#include "apps/maze-evil/input/menu_controls.hpp"

namespace micropixel {
class Application final {
   public:
    static constexpr KeyEvent Key(KeyPhase phase, KeyCode code) { return KeyEvent{TimePoint{}, code, phase, 0U}; }
    static constexpr TouchEvent Touch(TouchPhase phase, uint32_t id, int x, int y) {
        return TouchEvent{TimePoint{}, phase, id, x, y, false, 0U};
    }
};
}  // namespace micropixel

int main() {
    maze_break::game::RunRecord record;
    record.Start();
    assert(!record.Finish());
    record.Advance(12'345'001);
    assert(record.Finish() && record.best_ms() == 12346);
    assert(record.previous_best_ms() == 0);
    record.Start();
    record.Advance(20'000'000);
    assert(!record.Finish() && record.best_ms() == 12346);
    assert(record.previous_best_ms() == 12346);
    record.Start();
    record.Advance(12'346'000);
    assert(!record.Finish());
    record.Start();
    record.Advance(11'000'000);
    assert(record.Finish() && record.best_ms() == 11000);
    assert(record.previous_best_ms() == 12346);
    maze_break::game::RunRecord restored;
    restored.Restore(record.best_ms());
    restored.Start();
    restored.Advance(10'000'000);
    assert(restored.Finish() && restored.best_ms() == 10000);
    assert(restored.previous_best_ms() == 11000);
    restored.Start();
    assert(restored.previous_best_ms() == 10000);
    restored.Advance(UINT64_MAX);
    assert(restored.elapsed_ms() == UINT32_MAX && !restored.Finish());
    using micropixel::Application;
    using micropixel::TouchPhase;
    maze_break::input::MenuControls menu;
    assert(!menu.OnTouch(Application::Touch(TouchPhase::kUp, 1, 10, 10)));
    for (const int x : {10, 400, 700}) {
        assert(!menu.OnTouch(Application::Touch(TouchPhase::kDown, 1, x, 650)));
        assert(!menu.OnTouch(Application::Touch(TouchPhase::kDown, 2, x, 650)));
        assert(menu.OnTouch(Application::Touch(TouchPhase::kUp, 1, x, 650)));
        menu = {};
        assert(!menu.OnTouch(Application::Touch(TouchPhase::kUp, 2, x, 650)));
    }
    assert(!menu.OnTouch(Application::Touch(TouchPhase::kDown, 1, 10, 10)));
    assert(!menu.OnTouch(Application::Touch(TouchPhase::kCancel, 1, 10, 10)));
    assert(!menu.OnTouch(Application::Touch(TouchPhase::kUp, 1, 10, 10)));
    for (const auto key : {micropixel::KeyCode::kConfirm, micropixel::KeyCode::kBack, micropixel::KeyCode::kLeft}) {
        assert(!menu.OnKey(Application::Key(micropixel::KeyPhase::kUp, key)));
        assert(!menu.OnKey(Application::Key(micropixel::KeyPhase::kDown, key)));
        assert(!menu.OnKey(Application::Key(micropixel::KeyPhase::kRepeat, key)));
        assert(menu.OnKey(Application::Key(micropixel::KeyPhase::kUp, key)));
        assert(!menu.OnKey(Application::Key(micropixel::KeyPhase::kDown, key)));
        assert(!menu.OnKey(Application::Key(micropixel::KeyPhase::kCancel, key)));
        assert(!menu.OnKey(Application::Key(micropixel::KeyPhase::kUp, key)));
    }
    // The stick, look pad and fire button now come from the SDK VirtualGamepad;
    // see tools/tests/test_sdk_gamepad.cpp.
}
