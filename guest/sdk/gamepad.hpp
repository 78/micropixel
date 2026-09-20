#ifndef MICROPIXEL_SDK_GAMEPAD_HPP
#define MICROPIXEL_SDK_GAMEPAD_HPP

#include <stdint.h>

#include <optional>
#include <span>

#include "sdk/event.hpp"
#include "sdk/gamepad_style.hpp"
#include "sdk/geometry.hpp"
#include "sdk/math.hpp"

namespace micropixel {

// Which on-screen controls a game needs. Games declare the logical inputs;
// VirtualGamepad places them for the current screen and a GamepadSkin draws
// them. Physical gamepads feed the same GamepadState through OnKey(), so game
// code never branches on the input source.
enum class GamepadLayout : uint8_t {
    // Floating analog stick on the left half; the right half is ignored.
    kStickOnly,
    // Stick on the left, drag-to-look pad on the right. A short tap on the pad
    // presses `GamepadConfig::look_tap_button` (jump, interact, ...).
    kStickLook,
    // Stick on the left, 1..4 face buttons on the right.
    kStickButtons,
    // Stick, look pad and buttons: the buttons float over the look pad.
    kStickLookButtons,
    // Digital four-way pad on the left (stick with directions snapped to
    // -1/0/1) and face buttons on the right. Platformers.
    kDPadButtons,
    // Only face buttons, bottom-right. Rhythm and reaction games.
    kButtonsOnly,
};

// Preset button icons drawn by GamepadSkin. Letters follow the physical
// position names of KeyCode (South is the primary action on every pad).
enum class GamepadGlyph : uint8_t {
    kNone,
    kA,
    kB,
    kX,
    kY,
    kJump,
    kFire,
    kPunch,
    kSword,
    kShield,
    kRun,
    kInteract,
    kPause,
    kMenu,
    kArrowUp,
    kArrowDown,
    kArrowLeft,
    kArrowRight,
};

// Face buttons by physical position, matching KeyCode::kSouth..kNorth. The
// value doubles as the bit index in GamepadState masks.
enum class GamepadButton : uint8_t {
    kSouth = 0,
    kEast = 1,
    kWest = 2,
    kNorth = 3,
};

[[nodiscard]] constexpr uint8_t GamepadButtonBit(GamepadButton button) {
    return static_cast<uint8_t>(1U << static_cast<uint8_t>(button));
}

// Digital direction bits reported alongside the analog stick.
enum class GamepadDirection : uint8_t {
    kUp = 1U << 0U,
    kDown = 1U << 1U,
    kLeft = 1U << 2U,
    kRight = 1U << 3U,
};

[[nodiscard]] constexpr uint8_t GamepadDirectionBit(GamepadDirection direction) {
    return static_cast<uint8_t>(direction);
}

// When the on-screen controls are drawn. The Host will eventually drive this
// from a system setting and the physical gamepad connection; until then the
// SDK applies kAuto locally: hidden after key input until the next touch.
enum class GamepadOverlayPolicy : uint8_t {
    kAuto,
    kAlwaysVisible,
    kHidden,
};

struct GamepadButtonConfig final {
    GamepadGlyph glyph{GamepadGlyph::kNone};
    // Unspecified values use the layout preset. A centre is in bounds coordinates.
    std::optional<Point> center{};
    uint16_t radius{};
    GamepadButtonStyle style{};
};

struct GamepadConfig final {
    static constexpr uint8_t kMaxButtons = 4U;

    GamepadLayout layout{GamepadLayout::kStickLook};
    // Region in touch coordinates. Gamepad::Configure resolves {} to the current
    // logical canvas; standalone VirtualGamepad requires explicit bounds.
    Rect bounds{};
    // Ordered South, East, West, North. Configure copies this collection.
    std::span<const GamepadButtonConfig> buttons{};
    // Look-pad taps press this button; -1 disables. Ignored without a look pad.
    int8_t look_tap_button{static_cast<int8_t>(GamepadButton::kSouth)};
    // Geometry in `bounds` pixels; 0 derives from the short edge of `bounds`.
    uint16_t stick_radius{};    // finger travel for full deflection
    uint16_t stick_deadzone{};  // travel that still reads as centred
    // A floating stick centres on the first touch; a fixed one sits at its rest
    // position and only reacts inside twice its radius.
    bool floating_stick{true};
    // Look-pad taps: released within this time and travel.
    uint32_t tap_max_us{240'000U};
    uint16_t tap_max_travel{12U};
};

// Snapshot returned by VirtualGamepad::Consume(). Edge fields cover the
// interval since the previous Consume(); level fields describe the moment
// of the call.
struct GamepadState final {
    float stick_x{};  // -1..1, positive = right
    float stick_y{};  // -1..1, positive = down (screen space)
    bool stick_active{};
    int32_t look_dx{};  // accumulated look-pad drag, pixels
    int32_t look_dy{};
    bool look_tap{};
    uint8_t buttons_held{};
    uint8_t buttons_pressed{};
    uint8_t buttons_released{};
    uint8_t directions{};  // GamepadDirection bits; stick beyond half travel or keys
    // Physical gamepad only (EventType::kAxis): right stick and triggers. The
    // left stick already feeds stick_x/stick_y when no finger holds the pad.
    float right_x{};
    float right_y{};
    float left_trigger{};
    float right_trigger{};

    [[nodiscard]] constexpr bool Held(GamepadButton button) const {
        return (buttons_held & GamepadButtonBit(button)) != 0U;
    }
    [[nodiscard]] constexpr bool Pressed(GamepadButton button) const {
        return (buttons_pressed & GamepadButtonBit(button)) != 0U;
    }
    [[nodiscard]] constexpr bool Released(GamepadButton button) const {
        return (buttons_released & GamepadButtonBit(button)) != 0U;
    }
    [[nodiscard]] constexpr bool Direction(GamepadDirection direction) const {
        return (directions & GamepadDirectionBit(direction)) != 0U;
    }
};

// Touch-and-key state machine behind an on-screen gamepad. Header-only and
// free of Host calls, so games can unit-test their control mapping.
//
//   micropixel::VirtualGamepad pad;
//   const micropixel::GamepadButtonConfig buttons[] = {{.glyph = micropixel::GamepadGlyph::kFire}};
//   pad.Configure({.layout = micropixel::GamepadLayout::kStickLookButtons,
//                  .bounds = {0, 0, w, h}, .buttons = buttons});
//   // per event:  if (pad.OnEvent(event)) continue;   // the pad took it
//   // per frame:  const auto state = pad.Consume();
//
// Touch events arrive in buffer pixels for Surface-only Apps (the first
// DirectSurface adopts its buffer as the logical canvas), so `bounds` is simply
// the buffer rectangle. Mixed Scene + Surface Apps convert with
// DirectSurface::ToBuffer before calling OnTouch.
//
// Each contact keeps its role (stick, look pad or one button) until it lifts,
// even when it wanders into another region. Physical KeyCode::kUp..kRight
// drive the stick while no finger holds it; kSouth..kNorth (and kConfirm as
// South) drive the buttons.
class VirtualGamepad final {
   public:
    struct StickGeometry final {
        bool present{};
        bool active{};   // a finger holds the stick
        bool engaged{};  // a finger or the direction keys / left axis move it
        Point origin{};  // ring centre: the touch anchor, or the rest position
        Point knob{};    // knob centre, clamped to the ring
        int32_t radius{};
    };
    struct ButtonGeometry final {
        Point center{};
        int32_t radius{};
        bool held{};
        GamepadGlyph glyph{GamepadGlyph::kNone};
    };

    VirtualGamepad() = default;

    // Rejects invalid bounds, too many buttons or circles outside the bounds,
    // preserving the previous configuration. Success copies descriptors and releases contacts.
    bool Configure(const GamepadConfig& config) {
        if (config.bounds.empty() || config.buttons.size() > GamepadConfig::kMaxButtons) {
            return false;
        }
        const int32_t unit = math::Min(config.bounds.width, config.bounds.height);
        const auto count = static_cast<uint8_t>(config.buttons.size());
        const int32_t default_radius = math::Max(count >= 3U ? unit / 14 : unit / 11, 6);
        Point centers[GamepadConfig::kMaxButtons]{};
        int32_t radii[GamepadConfig::kMaxButtons]{};
        for (uint8_t index = 0U; index < count; ++index) {
            const GamepadButtonConfig& button = config.buttons[index];
            centers[index] = button.center.value_or(DefaultButtonCenter(config.bounds, count, index));
            radii[index] = button.radius != 0U ? button.radius : default_radius;
            const int64_t x = centers[index].x, y = centers[index].y, radius = radii[index];
            if (x - radius < config.bounds.x || y - radius < config.bounds.y ||
                x + radius > static_cast<int64_t>(config.bounds.x) + config.bounds.width ||
                y + radius > static_cast<int64_t>(config.bounds.y) + config.bounds.height) {
                return false;
            }
        }
        config_ = config;
        config_.buttons = {};  // Never retain a pointer into caller-owned descriptors.
        button_count_ = count;
        for (uint8_t index = 0U; index < count; ++index) {
            button_configs_[index] = config.buttons[index];
            button_centers_[index] = centers[index];
            button_radii_[index] = radii[index];
        }
        stick_radius_ = config.stick_radius != 0U ? config.stick_radius : math::Max(unit / 9, 8);
        stick_deadzone_ = config.stick_deadzone != 0U ? config.stick_deadzone : math::Max(stick_radius_ / 7, 2);
        stick_rest_ = {config.bounds.x + unit * 28 / 100, config.bounds.y + config.bounds.height - unit * 28 / 100};
        configured_ = true;
        Reset();
        return true;
    }

    [[nodiscard]] constexpr bool configured() const { return configured_; }
    // Snapshot with a view into this pad's owned descriptors, valid until reconfiguration.
    [[nodiscard]] GamepadConfig config() const {
        GamepadConfig snapshot = config_;
        snapshot.buttons = buttons();
        return snapshot;
    }
    [[nodiscard]] std::span<const GamepadButtonConfig> buttons() const { return {button_configs_, button_count_}; }

    // Releases every contact and key without emitting edges. Call on Resume.
    void Reset() {
        stick_ = {};
        look_ = {};
        for (Finger& finger : buttons_) {
            finger = {};
        }
        key_directions_ = 0U;
        key_buttons_ = 0U;
        held_ = 0U;
        pressed_ = 0U;
        released_ = 0U;
        look_dx_ = 0;
        look_dy_ = 0;
        look_tap_ = false;
        for (float& value : axes_) {
            value = 0.0F;
        }
        UpdateHeld();
    }

    // Routes one SDK event. True when the pad took it: a touch that starts on
    // one of its controls or belongs to a tracked contact, or a gamepad key.
    // Touches outside every control, Back/Menu keys and other event types
    // return false so the App handles them itself. Resume releases every
    // contact (and returns false).
    bool OnEvent(const Event& event) {
        switch (event.type()) {
            case EventType::kTouch:
                return OnTouch(*event.touch());
            case EventType::kKey:
                return OnKey(*event.key());
            case EventType::kAxis:
                return OnAxis(*event.axis());
            case EventType::kDeviceAdded:
            case EventType::kDeviceRemoved:
                OnDevice(*event.device(), event.type() == EventType::kDeviceAdded);
                return false;  // the App may still want to react to other devices
            case EventType::kResume:
                Reset();
                return false;
            default:
                return false;
        }
    }

    // Physical gamepad axis (Input 1.1). Always taken once configured.
    bool OnAxis(const AxisEvent& axis) {
        if (!configured_) {
            return false;
        }
        const uint16_t index = static_cast<uint16_t>(axis.axis());
        if (index == 0U || index > kAxisCount) {
            return false;
        }
        key_after_touch_ = true;
        axes_[index - 1U] = math::Clamp(axis.value(), -1.0F, 1.0F);
        return true;
    }

    // Devices added/removed with kind kGamepad track whether a physical pad is
    // connected; the on-screen controls hide while one is.
    void OnDevice(const DeviceEvent& device, bool added) {
        if (device.kind() != DeviceKind::kGamepad) {
            return;
        }
        physical_connected_ = added;
        if (added) {
            key_after_touch_ = true;
        }
    }

    [[nodiscard]] constexpr bool physical_connected() const { return physical_connected_; }

    // True when the contact belongs to the pad (see OnEvent).
    bool OnTouch(const TouchEvent& touch) {
        if (!configured_) {
            return false;
        }
        key_after_touch_ = false;
        const uint32_t id = touch.id();
        const Point position = touch.position();
        switch (touch.phase()) {
            case TouchPhase::kDown:
                if (FindFinger(id) != nullptr) {
                    return true;  // duplicate Down for a tracked contact
                }
                return AssignRole(id, position, touch.timestamp());
            case TouchPhase::kMove:
                return MoveFinger(id, position);
            case TouchPhase::kUp:
            case TouchPhase::kCancel:
                return LiftFinger(id, position, touch.timestamp(), touch.phase() == TouchPhase::kUp);
        }
        return false;
    }

    // True when the key maps to a gamepad control (see OnEvent).
    bool OnKey(const KeyEvent& key) {
        if (!configured_) {
            return false;
        }
        const bool down = key.phase() == KeyPhase::kDown || key.phase() == KeyPhase::kRepeat;
        const bool fresh = key.phase() == KeyPhase::kDown;
        uint8_t direction = 0U;
        int8_t button = -1;
        switch (key.code()) {
            case KeyCode::kUp:
                direction = GamepadDirectionBit(GamepadDirection::kUp);
                break;
            case KeyCode::kDown:
                direction = GamepadDirectionBit(GamepadDirection::kDown);
                break;
            case KeyCode::kLeft:
                direction = GamepadDirectionBit(GamepadDirection::kLeft);
                break;
            case KeyCode::kRight:
                direction = GamepadDirectionBit(GamepadDirection::kRight);
                break;
            case KeyCode::kConfirm:
            case KeyCode::kSouth:
                button = static_cast<int8_t>(GamepadButton::kSouth);
                break;
            case KeyCode::kEast:
                button = static_cast<int8_t>(GamepadButton::kEast);
                break;
            case KeyCode::kWest:
                button = static_cast<int8_t>(GamepadButton::kWest);
                break;
            case KeyCode::kNorth:
                button = static_cast<int8_t>(GamepadButton::kNorth);
                break;
            case KeyCode::kBack:
            case KeyCode::kMenu:
                return false;  // system keys stay with the App
        }
        key_after_touch_ = true;
        if (direction != 0U) {
            key_directions_ = down ? static_cast<uint8_t>(key_directions_ | direction)
                                   : static_cast<uint8_t>(key_directions_ & ~direction);
        }
        if (button >= 0) {
            const uint8_t bit = static_cast<uint8_t>(1U << button);
            if (down) {
                if (fresh && (key_buttons_ & bit) == 0U) {
                    pressed_ |= bit;
                }
                key_buttons_ |= bit;
            } else {
                if ((key_buttons_ & bit) != 0U && key.phase() == KeyPhase::kUp) {
                    released_ |= bit;
                }
                key_buttons_ &= static_cast<uint8_t>(~bit);
            }
        }
        UpdateHeld();
        return true;
    }

    // Current levels plus the edges and drag accumulated since the previous
    // call, which are then cleared.
    [[nodiscard]] GamepadState Consume() {
        GamepadState state = Peek();
        pressed_ = 0U;
        released_ = 0U;
        look_dx_ = 0;
        look_dy_ = 0;
        look_tap_ = false;
        return state;
    }

    // Like Consume() without clearing anything.
    [[nodiscard]] GamepadState Peek() const {
        GamepadState state{};
        ReadStick(state);
        state.look_dx = look_dx_;
        state.look_dy = look_dy_;
        state.look_tap = look_tap_;
        state.buttons_held = held_;
        state.buttons_pressed = pressed_;
        state.buttons_released = released_;
        state.right_x = Axis(GamepadAxis::kRightX);
        state.right_y = Axis(GamepadAxis::kRightY);
        state.left_trigger = math::Clamp(Axis(GamepadAxis::kLeftTrigger), 0.0F, 1.0F);
        state.right_trigger = math::Clamp(Axis(GamepadAxis::kRightTrigger), 0.0F, 1.0F);
        return state;
    }

    constexpr void set_overlay_policy(GamepadOverlayPolicy policy) { overlay_policy_ = policy; }
    [[nodiscard]] constexpr GamepadOverlayPolicy overlay_policy() const { return overlay_policy_; }
    // Whether a skin should draw the controls right now.
    [[nodiscard]] constexpr bool overlay_visible() const {
        switch (overlay_policy_) {
            case GamepadOverlayPolicy::kAlwaysVisible:
                return true;
            case GamepadOverlayPolicy::kHidden:
                return false;
            case GamepadOverlayPolicy::kAuto:
                break;
        }
        return !key_after_touch_;
    }

    [[nodiscard]] constexpr bool has_stick() const { return config_.layout != GamepadLayout::kButtonsOnly; }
    [[nodiscard]] constexpr bool has_look_pad() const {
        return config_.layout == GamepadLayout::kStickLook || config_.layout == GamepadLayout::kStickLookButtons;
    }
    [[nodiscard]] constexpr bool has_buttons() const {
        return config_.layout != GamepadLayout::kStickOnly && config_.layout != GamepadLayout::kStickLook &&
               button_count_ != 0U;
    }
    [[nodiscard]] constexpr bool digital_stick() const { return config_.layout == GamepadLayout::kDPadButtons; }
    [[nodiscard]] constexpr uint8_t button_count() const { return has_buttons() ? button_count_ : 0U; }
    [[nodiscard]] constexpr int32_t stick_radius() const { return stick_radius_; }
    [[nodiscard]] constexpr Point stick_rest() const { return stick_rest_; }
    // Right half of `bounds`; where look-pad contacts start.
    [[nodiscard]] constexpr Rect look_region() const {
        const Rect& b = config_.bounds;
        return {b.x + b.width / 2, b.y, b.width - b.width / 2, b.height};
    }

    // Geometry for skins, in `bounds` coordinates.
    [[nodiscard]] StickGeometry stick_geometry() const {
        StickGeometry geometry{};
        geometry.present = has_stick();
        geometry.radius = stick_radius_;
        if (!geometry.present) {
            return geometry;
        }
        geometry.active = stick_.down;
        geometry.engaged = stick_.down || key_directions_ != 0U || Axis(GamepadAxis::kLeftX) != 0.0F ||
                           Axis(GamepadAxis::kLeftY) != 0.0F;
        geometry.origin = stick_.down ? stick_.origin : stick_rest_;
        geometry.knob = geometry.origin;
        if (stick_.down) {
            int32_t dx = stick_.position.x - stick_.origin.x;
            int32_t dy = stick_.position.y - stick_.origin.y;
            const float length = math::Sqrt(static_cast<float>(dx * dx + dy * dy));
            if (length > static_cast<float>(stick_radius_)) {
                dx = static_cast<int32_t>(static_cast<float>(dx) * static_cast<float>(stick_radius_) / length);
                dy = static_cast<int32_t>(static_cast<float>(dy) * static_cast<float>(stick_radius_) / length);
            }
            geometry.knob = {geometry.origin.x + dx, geometry.origin.y + dy};
        } else if (key_directions_ != 0U) {
            // Keys move the drawn knob so the on-screen pad still gives feedback.
            const int32_t step = stick_radius_ * 3 / 4;
            geometry.knob.x +=
                Direction(GamepadDirection::kRight) ? step : (Direction(GamepadDirection::kLeft) ? -step : 0);
            geometry.knob.y +=
                Direction(GamepadDirection::kDown) ? step : (Direction(GamepadDirection::kUp) ? -step : 0);
        }
        return geometry;
    }

    [[nodiscard]] ButtonGeometry button_geometry(uint8_t index) const {
        ButtonGeometry geometry{};
        if (index >= button_count()) {
            return geometry;
        }
        geometry.center = button_centers_[index];
        geometry.radius = button_radii_[index];
        geometry.held = (held_ & static_cast<uint8_t>(1U << index)) != 0U;
        geometry.glyph = button_configs_[index].glyph;
        return geometry;
    }

   private:
    struct Finger final {
        bool down{};
        uint32_t id{};
        Point origin{};
        Point position{};
        int32_t travel{};
        TimePoint down_at{};
    };

    [[nodiscard]] constexpr bool Direction(GamepadDirection direction) const {
        return (key_directions_ & GamepadDirectionBit(direction)) != 0U;
    }

    [[nodiscard]] static Point DefaultButtonCenter(const Rect& bounds, uint8_t count, uint8_t index) {
        const int32_t unit = math::Min(bounds.width, bounds.height);
        const int32_t right = bounds.x + bounds.width;
        const int32_t bottom = bounds.y + bounds.height;
        if (count == 1U) {
            return {right - unit * 18 / 100, bottom - unit * 28 / 100};
        }
        const bool compact = count >= 3U;
        const Point centre{right - unit * (compact ? 27 : 30) / 100, bottom - unit * 30 / 100};
        const int32_t spread = unit * (compact ? 14 : 16) / 100;
        switch (index) {
            case 0U:
                return {centre.x, centre.y + spread};
            case 1U:
                return {centre.x + spread, centre.y};
            case 2U:
                return {centre.x - spread, centre.y};
            default:
                return {centre.x, centre.y - spread};
        }
    }

    [[nodiscard]] int8_t HitButton(Point position) const {
        for (uint8_t index = 0U; index < button_count(); ++index) {
            const int64_t hit_radius = button_radii_[index] * 5 / 4;
            const int64_t dx = static_cast<int64_t>(position.x) - button_centers_[index].x;
            const int64_t dy = static_cast<int64_t>(position.y) - button_centers_[index].y;
            if (dx * dx + dy * dy <= hit_radius * hit_radius) {
                return static_cast<int8_t>(index);
            }
        }
        return -1;
    }

    [[nodiscard]] Finger* FindFinger(uint32_t id) {
        if (stick_.down && stick_.id == id) {
            return &stick_;
        }
        if (look_.down && look_.id == id) {
            return &look_;
        }
        for (Finger& finger : buttons_) {
            if (finger.down && finger.id == id) {
                return &finger;
            }
        }
        return nullptr;
    }

    // Gives a new contact a role; false when it lands on nothing the pad owns
    // (or on a control another finger already holds).
    bool AssignRole(uint32_t id, Point position, TimePoint timestamp) {
        if (!config_.bounds.contains(position)) {
            return false;
        }
        const Finger contact{
            .down = true, .id = id, .origin = position, .position = position, .travel = 0, .down_at = timestamp};
        const int8_t button = HitButton(position);
        if (button >= 0) {
            if (buttons_[button].down) {
                return false;
            }
            buttons_[button] = contact;
            const uint8_t bit = static_cast<uint8_t>(1U << button);
            if ((held_ & bit) == 0U) {
                pressed_ |= bit;
            }
            UpdateHeld();
            return true;
        }
        const bool left_half = position.x < config_.bounds.x + config_.bounds.width / 2;
        if (has_stick() && left_half) {
            if (stick_.down) {
                return false;
            }
            if (!config_.floating_stick) {
                const int32_t dx = position.x - stick_rest_.x;
                const int32_t dy = position.y - stick_rest_.y;
                const int32_t reach = stick_radius_ * 2;
                if (dx * dx + dy * dy > reach * reach) {
                    return false;
                }
            }
            stick_ = contact;
            if (!config_.floating_stick) {
                stick_.origin = stick_rest_;
            }
            return true;
        }
        if (has_look_pad() && !left_half && !look_.down) {
            look_ = contact;
            return true;
        }
        return false;
    }

    // True when `id` is a tracked contact.
    bool MoveFinger(uint32_t id, Point position) {
        if (stick_.down && stick_.id == id) {
            stick_.position = position;
            return true;
        }
        if (look_.down && look_.id == id) {
            const int32_t dx = position.x - look_.position.x;
            const int32_t dy = position.y - look_.position.y;
            look_.travel += math::Abs(dx) + math::Abs(dy);
            look_dx_ += dx;
            look_dy_ += dy;
            look_.position = position;
            return true;
        }
        // Button contacts keep their button; sliding off does not release it.
        for (const Finger& finger : buttons_) {
            if (finger.down && finger.id == id) {
                return true;
            }
        }
        return false;
    }

    // True when `id` was a tracked contact.
    bool LiftFinger(uint32_t id, Point position, TimePoint timestamp, bool clean) {
        if (stick_.down && stick_.id == id) {
            stick_.down = false;
            return true;
        }
        if (look_.down && look_.id == id) {
            look_.position = position;
            const uint64_t held_us = timestamp.microseconds() - look_.down_at.microseconds();
            if (clean && look_.travel < static_cast<int32_t>(config_.tap_max_travel) && held_us < config_.tap_max_us) {
                look_tap_ = true;
                if (config_.look_tap_button >= 0 && config_.look_tap_button < GamepadConfig::kMaxButtons) {
                    const uint8_t bit = static_cast<uint8_t>(1U << config_.look_tap_button);
                    pressed_ |= bit;
                    released_ |= bit;
                }
            }
            look_.down = false;
            return true;
        }
        for (uint8_t index = 0U; index < GamepadConfig::kMaxButtons; ++index) {
            Finger& finger = buttons_[index];
            if (finger.down && finger.id == id) {
                finger.down = false;
                const uint8_t bit = static_cast<uint8_t>(1U << index);
                if (clean && (key_buttons_ & bit) == 0U) {
                    released_ |= bit;
                }
                UpdateHeld();
                return true;
            }
        }
        return false;
    }

    void UpdateHeld() {
        uint8_t touched = 0U;
        for (uint8_t index = 0U; index < GamepadConfig::kMaxButtons; ++index) {
            touched |= buttons_[index].down ? static_cast<uint8_t>(1U << index) : 0U;
        }
        held_ = static_cast<uint8_t>(touched | key_buttons_);
    }

    void ReadStick(GamepadState& state) const {
        if (!has_stick()) {
            return;
        }
        const float axis_x = Axis(GamepadAxis::kLeftX);
        const float axis_y = Axis(GamepadAxis::kLeftY);
        if (stick_.down) {
            state.stick_active = true;
            state.stick_x = Deflection(stick_.position.x - stick_.origin.x);
            state.stick_y = Deflection(stick_.position.y - stick_.origin.y);
        } else if (axis_x != 0.0F || axis_y != 0.0F) {
            state.stick_active = true;
            state.stick_x = axis_x;
            state.stick_y = axis_y;
        } else if (key_directions_ != 0U) {
            state.stick_active = true;
            state.stick_x =
                Direction(GamepadDirection::kRight) ? 1.0F : (Direction(GamepadDirection::kLeft) ? -1.0F : 0.0F);
            state.stick_y =
                Direction(GamepadDirection::kDown) ? 1.0F : (Direction(GamepadDirection::kUp) ? -1.0F : 0.0F);
        }
        state.directions = key_directions_;
        if (state.stick_x > 0.5F) {
            state.directions |= GamepadDirectionBit(GamepadDirection::kRight);
        } else if (state.stick_x < -0.5F) {
            state.directions |= GamepadDirectionBit(GamepadDirection::kLeft);
        }
        if (state.stick_y > 0.5F) {
            state.directions |= GamepadDirectionBit(GamepadDirection::kDown);
        } else if (state.stick_y < -0.5F) {
            state.directions |= GamepadDirectionBit(GamepadDirection::kUp);
        }
        if (digital_stick()) {
            state.stick_x = state.Direction(GamepadDirection::kRight)
                                ? 1.0F
                                : (state.Direction(GamepadDirection::kLeft) ? -1.0F : 0.0F);
            state.stick_y = state.Direction(GamepadDirection::kDown)
                                ? 1.0F
                                : (state.Direction(GamepadDirection::kUp) ? -1.0F : 0.0F);
        }
    }

    [[nodiscard]] constexpr float Axis(GamepadAxis axis) const { return axes_[static_cast<uint16_t>(axis) - 1U]; }

    [[nodiscard]] float Deflection(int32_t delta) const {
        if (math::Abs(delta) < stick_deadzone_) {
            return 0.0F;
        }
        return math::Clamp(static_cast<float>(delta) / static_cast<float>(stick_radius_), -1.0F, 1.0F);
    }

    GamepadConfig config_{};
    bool configured_{};
    GamepadOverlayPolicy overlay_policy_{GamepadOverlayPolicy::kAuto};
    bool key_after_touch_{};
    int32_t stick_radius_{};
    int32_t stick_deadzone_{};
    uint8_t button_count_{};
    GamepadButtonConfig button_configs_[GamepadConfig::kMaxButtons]{};
    int32_t button_radii_[GamepadConfig::kMaxButtons]{};
    Point stick_rest_{};
    Point button_centers_[GamepadConfig::kMaxButtons]{};
    Finger stick_{};
    Finger look_{};
    Finger buttons_[GamepadConfig::kMaxButtons]{};
    uint8_t key_directions_{};
    uint8_t key_buttons_{};
    uint8_t held_{};
    uint8_t pressed_{};
    uint8_t released_{};
    int32_t look_dx_{};
    int32_t look_dy_{};
    bool look_tap_{};
    static constexpr uint16_t kAxisCount = 6U;
    float axes_[kAxisCount]{};
    bool physical_connected_{};
};

// Runtime-owned gamepad, obtained from Application::gamepad(). Once
// Configure()d and enabled, the Guest Runtime feeds every touch, key, axis,
// gamepad device and Resume event to it while decoding events, before the App
// sees them; events it took are marked Event::gamepad_handled(). Games then
// only read Consume() each frame:
//
//   app.gamepad().Configure({.layout = GamepadLayout::kStickLook});
//   ...
//   const GamepadState state = app.gamepad().Consume();
//   skin.Draw(list, app.gamepad().pad());
//
// Disable it (set_enabled(false)) on menu pages so touches reach the App's UI.
// This is a Service View: copyable, without resources of its own.
class Gamepad final {
   public:
    constexpr Gamepad(const Gamepad&) noexcept = default;
    constexpr Gamepad& operator=(const Gamepad&) noexcept = default;

    // Configures the shared pad and enables it. Empty default bounds ({}) use
    // the current logical canvas. Configure the display / create surfaces first.
    // See VirtualGamepad::Configure for validation and descriptor ownership.
    bool Configure(const GamepadConfig& config) const;
    [[nodiscard]] bool configured() const;
    // While disabled the Runtime routes nothing to the pad and the pad holds
    // no contacts; re-enabling starts from a released state.
    void set_enabled(bool enabled) const;
    [[nodiscard]] bool enabled() const;
    // Releases every contact and key without emitting edges.
    void Reset() const;

    [[nodiscard]] GamepadState Consume() const;
    [[nodiscard]] GamepadState Peek() const;

    void set_overlay_policy(GamepadOverlayPolicy policy) const;
    [[nodiscard]] bool overlay_visible() const;
    [[nodiscard]] bool physical_connected() const;

    // The pad itself, for GamepadSkin and custom overlays.
    [[nodiscard]] const VirtualGamepad& pad() const;

   private:
    struct CapabilityToken final {
       private:
        constexpr CapabilityToken() = default;
        friend class Application;
    };

    explicit constexpr Gamepad(CapabilityToken) noexcept {}
    friend class Application;
};

}  // namespace micropixel

#endif
