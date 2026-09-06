#pragma once

namespace micropixel::host_ui::lvgl::square_common {

enum class HallLaunchBackgroundPlan {
    kCaptureVisibleHall,
    kPrepareCleanBaseline,
};

// Every launch captures the current carousel position and cover set. A
// suspended App's screenshot must be replaced by its idle cover first.
[[nodiscard]] constexpr HallLaunchBackgroundPlan PlanHallLaunchBackground(bool has_running_app) {
    return has_running_app ? HallLaunchBackgroundPlan::kPrepareCleanBaseline
                           : HallLaunchBackgroundPlan::kCaptureVisibleHall;
}

}  // namespace micropixel::host_ui::lvgl::square_common
