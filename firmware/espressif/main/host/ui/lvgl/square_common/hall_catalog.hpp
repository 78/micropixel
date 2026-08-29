#pragma once

#include <algorithm>
#include <cstdint>

#include "host/ui/system_ui.hpp"

namespace micropixel::host_ui::lvgl::square_common {

[[nodiscard]] inline uint32_t HallVisibleAppCount(const host_ui::HallModel& model) {
    return std::min(model.app_count, host_ui::kMaxHallApps);
}

[[nodiscard]] inline uint64_t HallCatalogSignature(const host_ui::HallModel& model, uint32_t app_count) {
    constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
    constexpr uint64_t kFnvPrime = 1099511628211ULL;
    uint64_t signature = (kFnvOffset ^ app_count) * kFnvPrime;
    for (uint32_t index = 0U; index < app_count; ++index) {
        const char* app_id = model.apps[index].app_id;
        if (app_id != nullptr) {
            while (*app_id != '\0') {
                signature = (signature ^ static_cast<uint8_t>(*app_id)) * kFnvPrime;
                ++app_id;
            }
        }
        signature = (signature ^ 0xffU) * kFnvPrime;
    }
    return signature;
}

// The two product boards retain different pixel backgrounds, but the decision
// to reuse an existing Hall scene must be identical. RunningStates may be a
// fixed std::array or a board-owned C array; no allocation is introduced.
template <typename RunningStates>
[[nodiscard]] bool HallResumeModelMatches(const host_ui::HallModel& model, uint32_t visible_count,
                                          uint64_t catalog_signature, uint32_t retained_app_count,
                                          uint64_t retained_catalog_signature, const RunningStates& retained_running,
                                          bool retained_launch_enabled, bool retained_firmware_update_available) {
    if (visible_count != retained_app_count || catalog_signature != retained_catalog_signature ||
        model.launch_enabled != retained_launch_enabled ||
        model.firmware_update_available != retained_firmware_update_available) {
        return false;
    }
    for (uint32_t index = 0U; index < visible_count; ++index) {
        if (retained_running[index] != model.apps[index].running) {
            return false;
        }
    }
    return true;
}

}  // namespace micropixel::host_ui::lvgl::square_common
