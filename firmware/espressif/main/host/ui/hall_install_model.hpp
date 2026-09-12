#pragma once

#include <algorithm>
#include <cstring>

#include "host/ui/system_ui.hpp"

namespace micropixel::host_ui {

// Existing apps retain their catalog index. A new installation is immediately
// visible at the front, including when the Hall has reached its display limit.
inline void ApplyHallInstallation(HallModel& model, const char* app_id, uint8_t progress_percent) {
    uint32_t index = 0U;
    for (; index < model.app_count; ++index) {
        if (model.apps[index].app_id != nullptr && std::strcmp(model.apps[index].app_id, app_id) == 0) {
            break;
        }
    }
    if (index == model.app_count) {
        model.app_count = std::min(model.app_count + 1U, kMaxHallApps);
        for (uint32_t position = model.app_count - 1U; position > 0U; --position) {
            model.apps[position] = model.apps[position - 1U];
        }
        index = 0U;
        model.apps[index] = {};
        model.apps[index].app_id = app_id;
        model.apps[index].display_name = app_id;
    }
    model.apps[index].installing = true;
    model.apps[index].install_progress_percent = progress_percent;
    if (model.status == HallStatus::kNoApps) {
        model.status = HallStatus::kReady;
    }
}

}  // namespace micropixel::host_ui
