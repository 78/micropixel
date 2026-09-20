#include "apps/tilt/tilt_input.hpp"

namespace tilt {

bool TiltInput::Initialize(micropixel::Application& app) {
    auto opened =
        app.sensors().OpenFirst<micropixel::Acceleration>(app.devices(), micropixel::Duration::Milliseconds(10U));
    if (!opened.has_value()) {
        return false;
    }
    accelerometer_ = static_cast<micropixel::Accelerometer&&>(opened.value());
    Recalibrate();
    return true;
}

bool TiltInput::Sample() {
    if (!available()) {
        return false;
    }
    auto result = accelerometer_.Read();
    if (!result.has_value()) {
        return result.error().code() == micropixel::ErrorCode::kWouldBlock;
    }
    (void)filter_.Sample(result->value, result->timestamp);
    return true;
}

}  // namespace tilt
