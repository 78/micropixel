// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <initializer_list>

#include "apps/maze-evil/game/run_record.hpp"
#include "apps/maze-evil/input/menu_controls.hpp"
#include "apps/maze-evil/input/touch_controls.hpp"

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
    for (const int size : {480, 720}) {
        maze_break::input::TouchControls input;
        input.Initialize(size, size);
        const auto overlay = input.overlay();
        const int x = overlay.fire_x;
        const int y = overlay.fire_y;
        assert(y == size / 2);
        // Bottom-right remains available for aiming while another finger fires.
        input.OnTouch(Application::Touch(TouchPhase::kDown, 10, x, size * 4 / 5));
        input.OnTouch(Application::Touch(TouchPhase::kMove, 10, x - 20, size * 4 / 5));
        input.OnTouch(Application::Touch(TouchPhase::kDown, 11, x, y));
        const auto two_fingers = input.Consume(0);
        assert(two_fingers.turn < 0 && two_fingers.fire);
        input.OnTouch(Application::Touch(TouchPhase::kUp, 10, x - 20, size * 4 / 5));
        input.OnTouch(Application::Touch(TouchPhase::kUp, 11, x, y));
        assert(!input.Consume(0).fire);
        const auto touch = [&](TouchPhase phase, uint32_t id, int tx, int ty) {
            input.OnTouch(Application::Touch(phase, id, tx, ty));
        };
        // A quick tap in the padding survives down/up within one frame.
        touch(TouchPhase::kDown, 1, x + overlay.fire_radius + 2, y);
        touch(TouchPhase::kUp, 1, x, y);
        assert(input.Consume(0).fire);
        assert(!input.Consume(0).fire);
        // Cancel must not synthesize a shot.
        touch(TouchPhase::kDown, 1, x, y);
        touch(TouchPhase::kCancel, 1, x, y);
        assert(!input.Consume(0).fire);
        // A look drag can cross into the fire pad without changing roles.
        touch(TouchPhase::kDown, 2, size * 3 / 4, size / 3);
        touch(TouchPhase::kMove, 2, x, y);
        auto controls = input.Consume(1'000'000);
        assert(controls.turn > 0 && !controls.fire);
        assert(!input.Consume(2'000'000).fire);
        // Three independent contacts can move, aim, and hold fire together.
        touch(TouchPhase::kDown, 3, size / 4, size / 2);
        touch(TouchPhase::kMove, 3, size / 4 + 30, size / 2 - 30);
        touch(TouchPhase::kDown, 4, x, y);
        touch(TouchPhase::kMove, 2, x - 20, y);
        controls = input.Consume(0);
        assert(controls.forward > 0 && controls.strafe > 0 && controls.turn < 0 && controls.fire);
        touch(TouchPhase::kUp, 99, x, y);
        assert(input.Consume(0).fire);
        touch(TouchPhase::kMove, 4, 0, 0);
        assert(input.Consume(0).fire);
        touch(TouchPhase::kUp, 4, 0, 0);
        touch(TouchPhase::kCancel, 2, x, y);
        touch(TouchPhase::kCancel, 3, 0, 0);
        controls = input.Consume(0);
        assert(!controls.fire && controls.forward == 0 && controls.strafe == 0 && controls.turn == 0);
    }
}
