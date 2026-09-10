// SPDX-License-Identifier: Apache-2.0
#include "sdk/micropixel.hpp"

int main() {
    micropixel::Application app;
    auto renderer = app.renderer();
    auto scene = renderer.CreateScene(micropixel::Color::Black()).value();
    (void)scene.CreateLabel({12, 16}, "SDK acceptance", micropixel::Color::White(), micropixel::SystemFont::kSmall)
        .value();
    auto touch_label =
        scene.CreateLabel({12, 56}, "Touch: waiting", micropixel::Color::White(), micropixel::SystemFont::kSmall)
            .value();
    auto key_label =
        scene.CreateLabel({12, 96}, "Key: waiting", micropixel::Color::White(), micropixel::SystemFont::kSmall).value();
    renderer.Present(scene).value();
    app.log().Info("acceptance: ready");
    app.Run([&](const micropixel::Event& event) {
        bool changed = false;
        if (const auto* touch = event.touch()) {
            if (touch->phase() == micropixel::TouchPhase::kDown) {
                touch_label.SetText("Touch: DOWN");
                app.log().Info("acceptance: touch down");
                changed = true;
            } else if (touch->phase() == micropixel::TouchPhase::kUp ||
                       touch->phase() == micropixel::TouchPhase::kCancel) {
                touch_label.SetText("Touch: UP");
                app.log().Info("acceptance: touch up");
                changed = true;
            }
        } else if (const auto* key = event.key()) {
            if (key->phase() == micropixel::KeyPhase::kDown) {
                key_label.SetText("Key: DOWN");
                app.log().Info("acceptance: key down");
                changed = true;
            } else if (key->phase() == micropixel::KeyPhase::kUp || key->phase() == micropixel::KeyPhase::kCancel) {
                key_label.SetText("Key: UP");
                app.log().Info("acceptance: key up");
                changed = true;
            }
        }
        if (changed || event.type() == micropixel::EventType::kResume) {
            renderer.Present(scene).value();
        }
    });
    return 0;
}
