// Runtime-owned gamepad behind Application::gamepad(). The Guest is single
// threaded with one App per session, so one VirtualGamepad instance serves the
// whole Guest; Application::WaitEventInternal offers every decoded event to it.
#include "sdk/gamepad.hpp"

#include "sdk/application.hpp"

namespace micropixel {

namespace {

// Lives in Guest static storage rather than on the 16 KiB Guest stack.
VirtualGamepad shared_pad{};
bool shared_pad_enabled{};

}  // namespace

bool Gamepad::Configure(const GamepadConfig& config) const {
    if (!shared_pad.Configure(config)) {
        return false;
    }
    shared_pad_enabled = true;
    return true;
}

bool Gamepad::configured() const { return shared_pad.configured(); }

void Gamepad::set_enabled(bool enabled) const {
    if (shared_pad_enabled == enabled) {
        return;
    }
    shared_pad_enabled = enabled;
    // Contacts made while disabled were never seen; those made before must not
    // survive into the next enabled phase either.
    shared_pad.Reset();
}

bool Gamepad::enabled() const { return shared_pad_enabled && shared_pad.configured(); }

void Gamepad::Reset() const { shared_pad.Reset(); }

GamepadState Gamepad::Consume() const { return shared_pad.Consume(); }

GamepadState Gamepad::Peek() const { return shared_pad.Peek(); }

void Gamepad::set_overlay_policy(GamepadOverlayPolicy policy) const { shared_pad.set_overlay_policy(policy); }

bool Gamepad::overlay_visible() const { return shared_pad.overlay_visible(); }

bool Gamepad::physical_connected() const { return shared_pad.physical_connected(); }

const VirtualGamepad& Gamepad::pad() const { return shared_pad; }

void Application::RouteToGamepad(Event& event) {
    if (!shared_pad.configured()) {
        return;
    }
    // Connection tracking and Resume housekeeping happen even while disabled;
    // touches, keys and axes only reach the pad while it is enabled.
    switch (event.type()) {
        case EventType::kResume:
        case EventType::kDeviceAdded:
        case EventType::kDeviceRemoved:
            (void)shared_pad.OnEvent(event);
            return;
        case EventType::kTouch:
        case EventType::kKey:
        case EventType::kAxis:
            if (shared_pad_enabled && shared_pad.OnEvent(event)) {
                event.gamepad_handled_ = true;
            }
            return;
        default:
            return;
    }
}

}  // namespace micropixel
