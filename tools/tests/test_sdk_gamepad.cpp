// SPDX-License-Identifier: Apache-2.0
// Host-side coverage of the SDK VirtualGamepad state machine: role stickiness
// per contact, stick deflection, look-pad drag and tap, button edges, physical
// key mapping and the overlay visibility policy.
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "sdk/gamepad.hpp"

namespace micropixel {
// The event constructors are private to the runtime; tests mint events the
// same way the maze/tomb tests do.
class Application final {
   public:
    static constexpr KeyEvent Key(KeyPhase phase, KeyCode code) { return KeyEvent{TimePoint{}, code, phase, 0U}; }
    static constexpr TouchEvent Touch(TouchPhase phase, uint32_t id, int x, int y, uint64_t at_us = 0U) {
        return TouchEvent{TimePoint{} + Duration::Microseconds(at_us), phase, id, x, y, false, 0U};
    }
    static constexpr Event TouchEventOf(TouchPhase phase, uint32_t id, int x, int y) {
        return Event{Touch(phase, id, x, y)};
    }
    static constexpr Event KeyEventOf(KeyPhase phase, KeyCode code) { return Event{Key(phase, code)}; }
    static constexpr Event Plain(EventType type) { return Event{type, TimePoint{}}; }
    static constexpr Event Axis(GamepadAxis axis, float value, uint32_t device = 7U) {
        return Event{AxisEvent{TimePoint{}, axis, value, DeviceId{device}}};
    }
    static constexpr Event Device(bool added, DeviceKind kind, uint32_t device = 7U) {
        return Event{added ? EventType::kDeviceAdded : EventType::kDeviceRemoved,
                     DeviceEvent{TimePoint{}, DeviceId{device}, kind, 1U}};
    }
};
}  // namespace micropixel

namespace {

using micropixel::Application;
using micropixel::GamepadButton;
using micropixel::GamepadConfig;
using micropixel::GamepadDirection;
using micropixel::GamepadGlyph;
using micropixel::GamepadLayout;
using micropixel::GamepadOverlayPolicy;
using micropixel::GamepadState;
using micropixel::KeyCode;
using micropixel::KeyPhase;
using micropixel::TouchPhase;
using micropixel::VirtualGamepad;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool Near(float value, float expected, float tolerance = 0.02F) {
    const float delta = value - expected;
    return (delta < 0.0F ? -delta : delta) <= tolerance;
}

VirtualGamepad MakePad(GamepadLayout layout, int size, uint8_t buttons) {
    VirtualGamepad pad;
    GamepadConfig config{};
    config.layout = layout;
    config.bounds = {0, 0, size, size};
    config.button_count = buttons;
    config.glyphs[0] = GamepadGlyph::kFire;
    Check(pad.Configure(config), "layout must configure for a square view");
    return pad;
}

void ConfigurationIsValidated() {
    VirtualGamepad pad;
    Check(!pad.Configure({.bounds = {0, 0, 0, 100}}), "empty bounds are rejected");
    Check(!pad.Configure({.bounds = {0, 0, 100, 100}, .button_count = 5U}), "more than four buttons are rejected");
    Check(!pad.configured(), "a rejected configuration leaves the pad unconfigured");
    pad.OnTouch(Application::Touch(TouchPhase::kDown, 1, 10, 10));
    Check(!pad.Consume().stick_active, "an unconfigured pad ignores input");
}

void StickDeflectsAndClamps() {
    for (const int size : {240, 480, 720}) {
        VirtualGamepad pad = MakePad(GamepadLayout::kStickOnly, size, 0U);
        const int radius = pad.stick_radius();
        Check(radius > 0, "stick radius derives from the view");
        const int x = size / 4;
        const int y = size / 2;
        pad.OnTouch(Application::Touch(TouchPhase::kDown, 1, x, y));
        GamepadState state = pad.Consume();
        Check(state.stick_active && state.stick_x == 0.0F && state.stick_y == 0.0F, "a fresh contact is centred");
        pad.OnTouch(Application::Touch(TouchPhase::kMove, 1, x + radius / 2, y - radius / 2));
        state = pad.Consume();
        Check(Near(state.stick_x, 0.5F) && Near(state.stick_y, -0.5F), "half travel reads as half deflection");
        Check(!state.Direction(GamepadDirection::kRight), "half deflection is below the digital threshold");
        pad.OnTouch(Application::Touch(TouchPhase::kMove, 1, x + radius * 3, y + radius * 3));
        state = pad.Consume();
        Check(state.stick_x == 1.0F && state.stick_y == 1.0F, "travel beyond the radius clamps to one");
        Check(state.Direction(GamepadDirection::kRight) && state.Direction(GamepadDirection::kDown),
              "full deflection sets the digital direction bits");
        const VirtualGamepad::StickGeometry geometry = pad.stick_geometry();
        const int dx = geometry.knob.x - geometry.origin.x;
        const int dy = geometry.knob.y - geometry.origin.y;
        Check(dx * dx + dy * dy <= (radius + 1) * (radius + 1), "the drawn knob stays on the ring");
        // Right-half contacts do not steal the stick and are ignored in kStickOnly.
        pad.OnTouch(Application::Touch(TouchPhase::kDown, 2, size * 3 / 4, y));
        pad.OnTouch(Application::Touch(TouchPhase::kMove, 2, size * 3 / 4 + 40, y));
        state = pad.Consume();
        Check(state.stick_x == 1.0F && state.look_dx == 0, "kStickOnly ignores the right half");
        pad.OnTouch(Application::Touch(TouchPhase::kUp, 1, 0, 0));
        Check(!pad.Consume().stick_active, "lifting the stick finger centres the stick");
        // Small movements inside the deadzone read as zero.
        pad.OnTouch(Application::Touch(TouchPhase::kDown, 3, x, y));
        pad.OnTouch(Application::Touch(TouchPhase::kMove, 3, x + 1, y));
        state = pad.Consume();
        Check(state.stick_active && state.stick_x == 0.0F, "the deadzone swallows jitter");
        pad.OnTouch(Application::Touch(TouchPhase::kCancel, 3, x, y));
    }
}

void LookPadDragsAndTaps() {
    const int size = 480;
    VirtualGamepad pad = MakePad(GamepadLayout::kStickLook, size, 0U);
    const int x = size * 3 / 4;
    const int y = size / 3;
    pad.OnTouch(Application::Touch(TouchPhase::kDown, 2, x, y, 1'000'000U));
    pad.OnTouch(Application::Touch(TouchPhase::kMove, 2, x + 30, y - 10, 1'020'000U));
    pad.OnTouch(Application::Touch(TouchPhase::kMove, 2, x + 50, y - 10, 1'040'000U));
    GamepadState state = pad.Consume();
    Check(state.look_dx == 50 && state.look_dy == -10, "look drag accumulates deltas since the last Consume");
    Check(!state.look_tap && state.buttons_pressed == 0U, "a drag is not a tap");
    // A drag may wander into the left half without becoming the stick.
    pad.OnTouch(Application::Touch(TouchPhase::kMove, 2, size / 4, y, 1'060'000U));
    state = pad.Consume();
    Check(!state.stick_active && state.look_dx < 0, "roles stick to the contact, not the region");
    pad.OnTouch(Application::Touch(TouchPhase::kUp, 2, size / 4, y, 1'500'000U));
    state = pad.Consume();
    Check(!state.look_tap, "a long drag released is not a tap");
    // Short tap: presses and releases the configured button in one frame.
    pad.OnTouch(Application::Touch(TouchPhase::kDown, 3, x, y, 2'000'000U));
    pad.OnTouch(Application::Touch(TouchPhase::kUp, 3, x + 2, y + 1, 2'100'000U));
    state = pad.Consume();
    Check(state.look_tap && state.Pressed(GamepadButton::kSouth) && state.Released(GamepadButton::kSouth),
          "a quick tap on the look pad presses the tap button");
    Check(!state.Held(GamepadButton::kSouth), "the tap does not leave the button held");
    Check(!pad.Consume().look_tap, "edges clear after Consume");
    // Cancel never taps; a slow release never taps.
    pad.OnTouch(Application::Touch(TouchPhase::kDown, 4, x, y, 3'000'000U));
    pad.OnTouch(Application::Touch(TouchPhase::kCancel, 4, x, y, 3'050'000U));
    Check(!pad.Consume().look_tap, "cancel is not a tap");
    pad.OnTouch(Application::Touch(TouchPhase::kDown, 5, x, y, 4'000'000U));
    pad.OnTouch(Application::Touch(TouchPhase::kUp, 5, x, y, 4'900'000U));
    Check(!pad.Consume().look_tap, "a long press is not a tap");
    // Disabling the tap button keeps the raw tap flag only.
    VirtualGamepad quiet;
    GamepadConfig config{};
    config.layout = GamepadLayout::kStickLook;
    config.bounds = {0, 0, size, size};
    config.look_tap_button = -1;
    Check(quiet.Configure(config), "look pad without a tap button configures");
    quiet.OnTouch(Application::Touch(TouchPhase::kDown, 6, x, y, 5'000'000U));
    quiet.OnTouch(Application::Touch(TouchPhase::kUp, 6, x, y, 5'050'000U));
    state = quiet.Consume();
    Check(state.look_tap && state.buttons_pressed == 0U, "look_tap_button = -1 reports the tap without a button");
}

void ButtonsTrackEdgesAndRoles() {
    for (const int size : {480, 720}) {
        VirtualGamepad pad = MakePad(GamepadLayout::kStickLookButtons, size, 1U);
        const VirtualGamepad::ButtonGeometry fire = pad.button_geometry(0U);
        Check(fire.radius > 0 && fire.glyph == GamepadGlyph::kFire, "button geometry exposes radius and glyph");
        Check(fire.center.x > size / 2 && fire.center.y > size / 2, "a single button sits bottom-right");
        Check(pad.button_geometry(1U).radius == 0, "absent buttons report empty geometry");
        const int x = fire.center.x;
        const int y = fire.center.y;
        // Bottom-right remains available for aiming while another finger fires.
        pad.OnTouch(Application::Touch(TouchPhase::kDown, 10, x, size / 4));
        pad.OnTouch(Application::Touch(TouchPhase::kMove, 10, x - 20, size / 4));
        pad.OnTouch(Application::Touch(TouchPhase::kDown, 11, x, y));
        GamepadState state = pad.Consume();
        Check(state.look_dx < 0 && state.Held(GamepadButton::kSouth) && state.Pressed(GamepadButton::kSouth),
              "look drag and button press coexist");
        Check(pad.button_geometry(0U).held, "held state is visible to skins");
        pad.OnTouch(Application::Touch(TouchPhase::kUp, 10, x - 20, size / 4));
        pad.OnTouch(Application::Touch(TouchPhase::kUp, 11, x, y));
        state = pad.Consume();
        Check(!state.Held(GamepadButton::kSouth) && state.Released(GamepadButton::kSouth), "release is an edge");
        // A tap in the hit padding survives down/up within one frame.
        pad.OnTouch(Application::Touch(TouchPhase::kDown, 1, x + fire.radius + 2, y));
        pad.OnTouch(Application::Touch(TouchPhase::kUp, 1, x, y));
        state = pad.Consume();
        Check(state.Pressed(GamepadButton::kSouth) && state.Released(GamepadButton::kSouth) &&
                  !state.Held(GamepadButton::kSouth),
              "a same-frame tap reports both edges");
        Check(pad.Consume().buttons_pressed == 0U, "edges clear after Consume");
        // Cancel does not synthesize a release edge.
        pad.OnTouch(Application::Touch(TouchPhase::kDown, 1, x, y));
        pad.OnTouch(Application::Touch(TouchPhase::kCancel, 1, x, y));
        state = pad.Consume();
        Check(state.Pressed(GamepadButton::kSouth) && !state.Released(GamepadButton::kSouth) &&
                  !state.Held(GamepadButton::kSouth),
              "cancel drops the contact without a release edge");
        // A look drag can cross into the button without changing roles.
        pad.OnTouch(Application::Touch(TouchPhase::kDown, 2, size * 3 / 4, size / 3));
        pad.OnTouch(Application::Touch(TouchPhase::kMove, 2, x, y));
        state = pad.Consume();
        Check(state.look_dx != 0 && !state.Held(GamepadButton::kSouth), "drag into the button does not press it");
        // Three independent contacts move, aim and hold fire together.
        pad.OnTouch(Application::Touch(TouchPhase::kDown, 3, size / 4, size / 2));
        pad.OnTouch(Application::Touch(TouchPhase::kMove, 3, size / 4 + 30, size / 2 - 30));
        pad.OnTouch(Application::Touch(TouchPhase::kDown, 4, x, y));
        pad.OnTouch(Application::Touch(TouchPhase::kMove, 2, x - 20, y));
        state = pad.Consume();
        Check(state.stick_x > 0 && state.stick_y < 0 && state.look_dx < 0 && state.Held(GamepadButton::kSouth),
              "stick, look and button work simultaneously");
        pad.OnTouch(Application::Touch(TouchPhase::kUp, 99, x, y));
        Check(pad.Consume().Held(GamepadButton::kSouth), "an unknown contact lifting changes nothing");
        pad.OnTouch(Application::Touch(TouchPhase::kMove, 4, 0, 0));
        Check(pad.Consume().Held(GamepadButton::kSouth), "sliding off a button keeps it held");
        pad.OnTouch(Application::Touch(TouchPhase::kUp, 4, 0, 0));
        pad.OnTouch(Application::Touch(TouchPhase::kCancel, 2, x, y));
        pad.OnTouch(Application::Touch(TouchPhase::kCancel, 3, 0, 0));
        state = pad.Consume();
        Check(!state.Held(GamepadButton::kSouth) && !state.stick_active && state.look_dx == 0, "everything lifts");
        // A second finger cannot claim an already pressed button.
        pad.OnTouch(Application::Touch(TouchPhase::kDown, 5, x, y));
        pad.OnTouch(Application::Touch(TouchPhase::kDown, 6, x, y));
        pad.OnTouch(Application::Touch(TouchPhase::kUp, 6, x, y));
        Check(pad.Consume().Held(GamepadButton::kSouth), "the first contact owns the button");
        pad.Reset();
        state = pad.Consume();
        Check(state.buttons_held == 0U && state.buttons_released == 0U, "Reset releases silently");
    }
}

void FourButtonDiamondStaysInsideBounds() {
    VirtualGamepad pad = MakePad(GamepadLayout::kStickButtons, 320, 4U);
    for (uint8_t index = 0U; index < 4U; ++index) {
        const VirtualGamepad::ButtonGeometry button = pad.button_geometry(index);
        Check(button.center.x - button.radius >= 160 && button.center.x + button.radius <= 320 &&
                  button.center.y - button.radius >= 0 && button.center.y + button.radius <= 320,
              "buttons fit inside the right half");
        for (uint8_t other = 0U; other < index; ++other) {
            const VirtualGamepad::ButtonGeometry previous = pad.button_geometry(other);
            const int dx = button.center.x - previous.center.x;
            const int dy = button.center.y - previous.center.y;
            const int minimum = (button.radius * 5 / 4) * 2;
            Check(dx * dx + dy * dy >= minimum * minimum, "button hit areas do not overlap");
        }
    }
    Check(pad.button_geometry(0U).center.y > pad.button_geometry(3U).center.y, "South sits below North");
    Check(pad.button_geometry(1U).center.x > pad.button_geometry(2U).center.x, "East sits right of West");
}

void KeysDriveTheSameState() {
    VirtualGamepad pad = MakePad(GamepadLayout::kStickButtons, 480, 2U);
    pad.OnKey(Application::Key(KeyPhase::kDown, KeyCode::kRight));
    pad.OnKey(Application::Key(KeyPhase::kDown, KeyCode::kUp));
    GamepadState state = pad.Consume();
    Check(state.stick_active && state.stick_x == 1.0F && state.stick_y == -1.0F, "direction keys deflect the stick");
    Check(state.Direction(GamepadDirection::kRight) && state.Direction(GamepadDirection::kUp), "keys set directions");
    Check(pad.stick_geometry().knob.x > pad.stick_geometry().origin.x, "the drawn knob follows the keys");
    pad.OnKey(Application::Key(KeyPhase::kUp, KeyCode::kRight));
    state = pad.Consume();
    Check(state.stick_x == 0.0F && state.stick_y == -1.0F, "releasing one key keeps the other");
    pad.OnKey(Application::Key(KeyPhase::kUp, KeyCode::kUp));
    Check(!pad.Consume().stick_active, "no keys, no stick");
    // Touch wins over keys while a finger holds the stick.
    pad.OnKey(Application::Key(KeyPhase::kDown, KeyCode::kLeft));
    pad.OnTouch(Application::Touch(TouchPhase::kDown, 1, 100, 300));
    pad.OnTouch(Application::Touch(TouchPhase::kMove, 1, 100 + pad.stick_radius(), 300));
    state = pad.Consume();
    Check(state.stick_x == 1.0F, "a touched stick overrides the direction keys");
    pad.OnTouch(Application::Touch(TouchPhase::kUp, 1, 0, 0));
    pad.OnKey(Application::Key(KeyPhase::kUp, KeyCode::kLeft));
    // Face buttons and Confirm map to positions; Repeat is not a new press.
    pad.OnKey(Application::Key(KeyPhase::kDown, KeyCode::kConfirm));
    state = pad.Consume();
    Check(state.Pressed(GamepadButton::kSouth) && state.Held(GamepadButton::kSouth), "Confirm is South");
    pad.OnKey(Application::Key(KeyPhase::kRepeat, KeyCode::kConfirm));
    state = pad.Consume();
    Check(!state.Pressed(GamepadButton::kSouth) && state.Held(GamepadButton::kSouth), "Repeat keeps holding");
    pad.OnKey(Application::Key(KeyPhase::kUp, KeyCode::kConfirm));
    Check(pad.Consume().Released(GamepadButton::kSouth), "key Up releases");
    pad.OnKey(Application::Key(KeyPhase::kDown, KeyCode::kEast));
    pad.OnKey(Application::Key(KeyPhase::kDown, KeyCode::kNorth));
    state = pad.Consume();
    Check(state.Held(GamepadButton::kEast) && state.Held(GamepadButton::kNorth), "face keys map by position");
    pad.OnKey(Application::Key(KeyPhase::kCancel, KeyCode::kEast));
    pad.OnKey(Application::Key(KeyPhase::kUp, KeyCode::kNorth));
    state = pad.Consume();
    Check(!state.Released(GamepadButton::kEast) && state.Released(GamepadButton::kNorth),
          "key Cancel drops without a release edge");
    // System keys are left to the App.
    pad.OnKey(Application::Key(KeyPhase::kDown, KeyCode::kBack));
    pad.OnKey(Application::Key(KeyPhase::kDown, KeyCode::kMenu));
    Check(pad.Consume().buttons_held == 0U, "Back and Menu are not gamepad buttons");
    // A key held while a finger also holds the button releases only when both lift.
    const VirtualGamepad::ButtonGeometry south = pad.button_geometry(0U);
    pad.OnKey(Application::Key(KeyPhase::kDown, KeyCode::kSouth));
    pad.OnTouch(Application::Touch(TouchPhase::kDown, 7, south.center.x, south.center.y));
    state = pad.Consume();
    Check(state.buttons_pressed == GamepadButtonBit(GamepadButton::kSouth), "one press edge for key plus touch");
    pad.OnTouch(Application::Touch(TouchPhase::kUp, 7, south.center.x, south.center.y));
    state = pad.Consume();
    Check(state.Held(GamepadButton::kSouth) && !state.Released(GamepadButton::kSouth), "key still holds");
    pad.OnKey(Application::Key(KeyPhase::kUp, KeyCode::kSouth));
    Check(pad.Consume().Released(GamepadButton::kSouth), "the last source releases");
}

void DigitalPadSnapsDirections() {
    VirtualGamepad pad = MakePad(GamepadLayout::kDPadButtons, 480, 2U);
    const int radius = pad.stick_radius();
    pad.OnTouch(Application::Touch(TouchPhase::kDown, 1, 120, 240));
    pad.OnTouch(Application::Touch(TouchPhase::kMove, 1, 120 + radius * 3 / 4, 240 + radius / 4));
    GamepadState state = pad.Consume();
    Check(state.stick_x == 1.0F && state.stick_y == 0.0F, "the digital pad snaps to whole directions");
    Check(state.Direction(GamepadDirection::kRight) && !state.Direction(GamepadDirection::kDown),
          "minor axis below half travel is dropped");
    pad.OnTouch(Application::Touch(TouchPhase::kUp, 1, 0, 0));
}

void ButtonsOnlyIgnoresTheLeftHalf() {
    VirtualGamepad pad = MakePad(GamepadLayout::kButtonsOnly, 480, 2U);
    Check(!pad.has_stick() && !pad.has_look_pad() && pad.button_count() == 2U, "kButtonsOnly exposes only buttons");
    pad.OnTouch(Application::Touch(TouchPhase::kDown, 1, 60, 240));
    pad.OnTouch(Application::Touch(TouchPhase::kMove, 1, 160, 240));
    GamepadState state = pad.Consume();
    Check(!state.stick_active && state.look_dx == 0, "left-half touches are ignored without a stick");
    Check(!pad.stick_geometry().present, "skins skip the absent stick");
}

void OverlayPolicyFollowsTheLastInputSource() {
    VirtualGamepad pad = MakePad(GamepadLayout::kStickButtons, 480, 1U);
    Check(pad.overlay_visible(), "the overlay starts visible");
    pad.OnKey(Application::Key(KeyPhase::kDown, KeyCode::kSouth));
    pad.OnKey(Application::Key(KeyPhase::kUp, KeyCode::kSouth));
    Check(!pad.overlay_visible(), "key input hides the overlay under kAuto");
    pad.OnTouch(Application::Touch(TouchPhase::kDown, 1, 100, 300));
    Check(pad.overlay_visible(), "a touch brings the overlay back");
    pad.OnTouch(Application::Touch(TouchPhase::kUp, 1, 100, 300));
    pad.OnKey(Application::Key(KeyPhase::kDown, KeyCode::kMenu));
    Check(pad.overlay_visible(), "system keys do not count as gamepad use");
    pad.OnKey(Application::Key(KeyPhase::kDown, KeyCode::kLeft));
    pad.set_overlay_policy(GamepadOverlayPolicy::kAlwaysVisible);
    Check(pad.overlay_visible(), "kAlwaysVisible overrides key use");
    pad.set_overlay_policy(GamepadOverlayPolicy::kHidden);
    pad.OnTouch(Application::Touch(TouchPhase::kDown, 2, 100, 300));
    Check(!pad.overlay_visible(), "kHidden overrides touch use");
    pad.set_overlay_policy(GamepadOverlayPolicy::kAuto);
    Check(pad.overlay_visible(), "kAuto remembers that the last input was a touch");
}

void FixedStickOnlyReactsNearItsRest() {
    VirtualGamepad pad;
    GamepadConfig config{};
    config.layout = GamepadLayout::kStickOnly;
    config.bounds = {0, 0, 480, 480};
    config.floating_stick = false;
    Check(pad.Configure(config), "fixed stick configures");
    const micropixel::Point rest = pad.stick_rest();
    pad.OnTouch(Application::Touch(TouchPhase::kDown, 1, 10, 10));
    Check(!pad.Consume().stick_active, "far from the rest position nothing happens");
    pad.OnTouch(Application::Touch(TouchPhase::kDown, 2, rest.x + pad.stick_radius(), rest.y));
    GamepadState state = pad.Consume();
    Check(state.stick_active && state.stick_x == 1.0F, "a fixed stick measures from its rest position");
    Check(pad.stick_geometry().origin == rest, "the ring stays at the rest position");
}

void OnEventReportsOwnership() {
    using micropixel::EventType;
    const int size = 480;
    VirtualGamepad pad = MakePad(GamepadLayout::kStickButtons, size, 1U);
    const VirtualGamepad::ButtonGeometry fire = pad.button_geometry(0U);
    // Right-half touches away from the button belong to the App in this layout.
    Check(!pad.OnEvent(Application::TouchEventOf(TouchPhase::kDown, 1, size * 3 / 4, 40)), "no look pad: not taken");
    Check(!pad.OnEvent(Application::TouchEventOf(TouchPhase::kMove, 1, size * 3 / 4, 60)), "untracked move: not taken");
    Check(!pad.OnEvent(Application::TouchEventOf(TouchPhase::kUp, 1, size * 3 / 4, 60)), "untracked lift: not taken");
    // Stick and button contacts are taken for their whole life.
    Check(pad.OnEvent(Application::TouchEventOf(TouchPhase::kDown, 2, size / 4, size / 2)), "stick down taken");
    Check(pad.OnEvent(Application::TouchEventOf(TouchPhase::kMove, 2, size * 3 / 4, size / 2)), "stick move taken");
    Check(pad.OnEvent(Application::TouchEventOf(TouchPhase::kUp, 2, size * 3 / 4, size / 2)), "stick lift taken");
    Check(pad.OnEvent(Application::TouchEventOf(TouchPhase::kDown, 3, fire.center.x, fire.center.y)), "button down");
    Check(!pad.OnEvent(Application::TouchEventOf(TouchPhase::kDown, 4, fire.center.x, fire.center.y)),
          "a second finger on a held button is not taken");
    Check(pad.OnEvent(Application::TouchEventOf(TouchPhase::kMove, 3, 0, 0)), "button move taken");
    Check(pad.OnEvent(Application::TouchEventOf(TouchPhase::kCancel, 3, 0, 0)), "button cancel taken");
    // Outside the bounds nothing is taken.
    Check(!pad.OnEvent(Application::TouchEventOf(TouchPhase::kDown, 5, -10, 10)), "outside bounds: not taken");
    // Keys: gamepad codes yes, system codes no.
    Check(pad.OnEvent(Application::KeyEventOf(KeyPhase::kDown, KeyCode::kLeft)), "direction key taken");
    Check(pad.OnEvent(Application::KeyEventOf(KeyPhase::kUp, KeyCode::kLeft)), "direction key release taken");
    Check(pad.OnEvent(Application::KeyEventOf(KeyPhase::kDown, KeyCode::kConfirm)), "confirm taken as South");
    Check(!pad.OnEvent(Application::KeyEventOf(KeyPhase::kDown, KeyCode::kBack)), "Back left to the App");
    Check(!pad.OnEvent(Application::KeyEventOf(KeyPhase::kDown, KeyCode::kMenu)), "Menu left to the App");
    // Resume releases everything and is not consumed.
    Check(pad.Peek().Held(GamepadButton::kSouth), "confirm is held before Resume");
    Check(!pad.OnEvent(Application::Plain(EventType::kResume)), "Resume passes through");
    Check(!pad.Peek().Held(GamepadButton::kSouth) && pad.Consume().buttons_released == 0U, "Resume releases silently");
    Check(!pad.OnEvent(Application::Plain(EventType::kStop)), "unrelated events pass through");
    // The look pad takes the whole right half.
    VirtualGamepad look = MakePad(GamepadLayout::kStickLook, size, 0U);
    Check(look.OnEvent(Application::TouchEventOf(TouchPhase::kDown, 6, size * 3 / 4, 40)), "look pad taken");
    Check(!look.OnEvent(Application::TouchEventOf(TouchPhase::kDown, 7, size * 3 / 4, 80)),
          "a second look finger is not taken");
    // An unconfigured pad never takes anything.
    VirtualGamepad idle;
    Check(!idle.OnEvent(Application::TouchEventOf(TouchPhase::kDown, 1, 10, 10)), "unconfigured: not taken");
    Check(!idle.OnEvent(Application::KeyEventOf(KeyPhase::kDown, KeyCode::kSouth)), "unconfigured key: not taken");
}

void PhysicalAxesFeedTheSameState() {
    using micropixel::DeviceKind;
    using micropixel::GamepadAxis;
    VirtualGamepad pad = MakePad(GamepadLayout::kStickLookButtons, 480, 1U);
    Check(!pad.physical_connected(), "no gamepad at start");
    Check(pad.OnEvent(Application::Axis(GamepadAxis::kLeftX, 0.75F)), "axis events are taken");
    Check(pad.OnEvent(Application::Axis(GamepadAxis::kLeftY, -1.5F)), "out-of-range values are clamped, not rejected");
    GamepadState state = pad.Consume();
    Check(state.stick_active && Near(state.stick_x, 0.75F) && state.stick_y == -1.0F, "the left stick drives the pad");
    Check(state.Direction(GamepadDirection::kRight) && state.Direction(GamepadDirection::kUp), "axes set directions");
    Check(!pad.overlay_visible(), "axis input hides the overlay like keys");
    Check(pad.OnEvent(Application::Axis(GamepadAxis::kRightX, -0.5F)), "right stick taken");
    Check(pad.OnEvent(Application::Axis(GamepadAxis::kRightTrigger, 0.4F)), "trigger taken");
    Check(pad.OnEvent(Application::Axis(GamepadAxis::kLeftTrigger, -0.4F)), "negative trigger taken");
    state = pad.Peek();
    Check(Near(state.right_x, -0.5F) && Near(state.right_trigger, 0.4F) && state.left_trigger == 0.0F,
          "right stick and triggers are reported separately; triggers clamp at zero");
    // Touch beats the axis while a finger holds the stick; keys lose to the axis.
    pad.OnTouch(Application::Touch(TouchPhase::kDown, 1, 100, 300));
    pad.OnTouch(Application::Touch(TouchPhase::kMove, 1, 100 - pad.stick_radius(), 300));
    Check(pad.Consume().stick_x == -1.0F, "a touched stick overrides the axis");
    pad.OnTouch(Application::Touch(TouchPhase::kUp, 1, 0, 0));
    pad.OnKey(Application::Key(KeyPhase::kDown, KeyCode::kRight));
    Check(Near(pad.Consume().stick_x, 0.75F), "the axis overrides the direction keys");
    pad.OnKey(Application::Key(KeyPhase::kUp, KeyCode::kRight));
    pad.OnEvent(Application::Axis(GamepadAxis::kLeftX, 0.0F));
    pad.OnEvent(Application::Axis(GamepadAxis::kLeftY, 0.0F));
    Check(!pad.Consume().stick_active, "centred axes release the stick");
    // Device events track connection and are never consumed.
    Check(!pad.OnEvent(Application::Device(true, DeviceKind::kSensor)), "other devices pass through");
    Check(!pad.physical_connected(), "a sensor is not a gamepad");
    Check(!pad.OnEvent(Application::Device(true, DeviceKind::kGamepad)), "gamepad added passes through");
    Check(pad.physical_connected() && !pad.overlay_visible(), "a connected gamepad hides the overlay");
    pad.OnTouch(Application::Touch(TouchPhase::kDown, 2, 100, 300));
    Check(pad.overlay_visible(), "touching brings the overlay back even while connected");
    pad.OnTouch(Application::Touch(TouchPhase::kUp, 2, 100, 300));
    Check(!pad.OnEvent(Application::Device(false, DeviceKind::kGamepad)), "gamepad removed passes through");
    Check(!pad.physical_connected(), "removal clears the connection");
    pad.OnEvent(Application::Axis(GamepadAxis::kLeftX, 1.0F));
    pad.Reset();
    Check(!pad.Consume().stick_active, "Reset clears the axes");
    VirtualGamepad idle;
    Check(!idle.OnEvent(Application::Axis(GamepadAxis::kLeftX, 1.0F)), "unconfigured: axes not taken");
}

}  // namespace

int main() {
    ConfigurationIsValidated();
    StickDeflectsAndClamps();
    LookPadDragsAndTaps();
    ButtonsTrackEdgesAndRoles();
    FourButtonDiamondStaysInsideBounds();
    KeysDriveTheSameState();
    DigitalPadSnapsDirections();
    ButtonsOnlyIgnoresTheLeftHalf();
    OverlayPolicyFollowsTheLastInputSource();
    FixedStickOnlyReactsNearItsRest();
    OnEventReportsOwnership();
    PhysicalAxesFeedTheSameState();
    std::cout << "sdk gamepad tests passed\n";
    return 0;
}
