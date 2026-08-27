#pragma once

namespace micropixel::platform::lvgl::square_720 {

enum class HallLaunchBackgroundPlan {
    kCaptureVisibleHall,
    kPrepareCleanBaseline,
    kReuseCleanBaseline,
};

[[nodiscard]] constexpr HallLaunchBackgroundPlan PlanHallLaunchBackground(bool has_running_app, bool has_background) {
    if (!has_running_app) {
        return HallLaunchBackgroundPlan::kCaptureVisibleHall;
    }
    return has_background ? HallLaunchBackgroundPlan::kReuseCleanBaseline
                          : HallLaunchBackgroundPlan::kPrepareCleanBaseline;
}

}  // namespace micropixel::platform::lvgl::square_720
