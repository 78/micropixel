#ifndef MICROPIXEL_HOST_UI_SYSTEM_SETTINGS_STORE_HPP
#define MICROPIXEL_HOST_UI_SYSTEM_SETTINGS_STORE_HPP

#include "host_ui/system_ui.hpp"
#include "nvs.h"

namespace micropixel::host_ui {

// Owns the Host UI settings namespace in the dedicated sys_store NVS
// partition. Guest KV data remains isolated in runtime_nvs.
class SystemSettingsStore final {
   public:
    SystemSettingsStore() = default;
    SystemSettingsStore(const SystemSettingsStore&) = delete;
    SystemSettingsStore& operator=(const SystemSettingsStore&) = delete;
    ~SystemSettingsStore();

    [[nodiscard]] bool Initialize();
    [[nodiscard]] bool Load(StatusLayerModel& model) const;
    [[nodiscard]] bool Save(const StatusLayerModel& model) const;
    [[nodiscard]] bool ready() const { return handle_ != 0U; }  // NOLINT(readability-identifier-naming)

   private:
    nvs_handle_t handle_{};
};

}  // namespace micropixel::host_ui

#endif
