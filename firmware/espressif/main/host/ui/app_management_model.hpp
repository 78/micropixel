// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

#include "host/ui/system_ui.hpp"

namespace micropixel::host_ui {

inline bool AppManagementBusy(const AppManagementModel& model) {
    return model.uninstall_state == AppUninstallState::kPending ||
           host::StoreUpdateRequestBusy(model.update_request_state);
}

// Latch independently in the UI callback and Host before touching storage.
inline bool BeginAppUninstall(AppManagementModel& model, uint32_t index) {
    if (AppManagementBusy(model) || !model.uninstall_available || index >= model.app_count) return false;
    model.uninstall_state = AppUninstallState::kPending;
    model.uninstall_app_index = index;
    return true;
}

// Background Store checks do not change a Hall sheet unless its visible
// content or available actions change. Preserve its objects and animation.
inline bool AppManagementActionSheetMatches(const AppManagementModel& left, const AppManagementModel& right) {
    if (left.action_app_index != right.action_app_index || left.action_app_index >= left.app_count ||
        right.action_app_index >= right.app_count || left.update_request_state != right.update_request_state ||
        left.launch_available != right.launch_available || left.uninstall_available != right.uninstall_available ||
        left.external_storage_status != right.external_storage_status ||
        left.uninstall_state != right.uninstall_state || left.uninstall_app_index != right.uninstall_app_index) {
        return false;
    }
    const auto& a = left.apps[left.action_app_index];
    const auto& b = right.apps[right.action_app_index];
    const auto text = [](const char* value) { return std::string_view(value != nullptr ? value : ""); };
    return text(a.app_id) == text(b.app_id) && text(a.display_name) == text(b.display_name) &&
           a.bundle_size_kib == b.bundle_size_kib && a.external_storage == b.external_storage &&
           a.update_version == b.update_version;
}

}  // namespace micropixel::host_ui
