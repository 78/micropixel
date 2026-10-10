// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/m5stack-tab5/inertial_axis.hpp"

namespace micropixel::platform::m5stack_tab5 {

esp_err_t Tab5InertialAxis::Initialize(i2c_master_bus_handle_t bus) { return source_.Initialize(bus); }

esp_err_t Tab5InertialAxis::Configure(uint32_t interval_us) { return source_.Configure(interval_us); }

esp_err_t Tab5InertialAxis::Suspend() { return source_.Suspend(); }

esp_err_t Tab5InertialAxis::Read(float (&values)[3]) {
    float raw[3]{};
    const esp_err_t status = source_.Read(raw);
    if (status != ESP_OK) {
        return status;
    }
    const Axis axes[3] = {mapping_.x, mapping_.y, mapping_.z};
    for (uint32_t index = 0U; index < 3U; ++index) {
        const float value = raw[axes[index].source];
        values[index] = axes[index].inverted ? -value : value;
    }
    return ESP_OK;
}

bool Tab5InertialAxis::available() const { return source_.available(); }

uint32_t Tab5InertialAxis::min_interval_us() const { return source_.min_interval_us(); }

}  // namespace micropixel::platform::m5stack_tab5
