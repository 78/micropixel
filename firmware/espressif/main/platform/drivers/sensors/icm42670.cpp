#include "platform/drivers/sensors/icm42670.hpp"

#include "esp_check.h"
#include "esp_log.h"

namespace micropixel::platform::drivers {
namespace {

constexpr char kTag[] = "icm42670";
constexpr uint8_t kAddress = ICM42670_I2C_ADDRESS;
constexpr float kGravityMetersPerSecondSquared = 9.80665F;
constexpr float kRadiansPerDegree = 0.017453292519943295F;

}  // namespace

Icm42670::~Icm42670() {
    if (handle_ != nullptr) {
        icm42670_delete(handle_);
    }
}

esp_err_t Icm42670::Initialize(i2c_master_bus_handle_t bus) {
    if (bus == nullptr || handle_ != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(icm42670_create(bus, kAddress, &handle_), kTag, "create ICM-42607-P failed");
    const esp_err_t status = ApplyConfiguration() == ESP_OK ? ApplyPowerState() : ESP_FAIL;
    if (status != ESP_OK) {
        icm42670_delete(handle_);
        handle_ = nullptr;
        return status;
    }
    uint8_t device_id = 0U;
    const esp_err_t id_status = icm42670_get_deviceid(handle_, &device_id);
    if (id_status != ESP_OK) {
        ESP_LOGW(kTag, "read device id failed: %s", esp_err_to_name(id_status));
        icm42670_delete(handle_);
        handle_ = nullptr;
        return id_status;
    }
    ESP_LOGI(kTag, "ready: WHO_AM_I=0x%02x", device_id);
    return ESP_OK;
}

esp_err_t Icm42670::Configure(Kind kind, uint32_t interval_us) {
    if (!available()) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool previous_acceleration = acceleration_active_;
    const bool previous_angular_velocity = angular_velocity_active_;
    const icm42670_cfg_t previous_configuration = configuration_;
    if (kind == Kind::kAcceleration) {
        configuration_.acce_odr = AccelerationOdr(interval_us);
        acceleration_active_ = true;
    } else {
        configuration_.gyro_odr = GyroscopeOdr(interval_us);
        angular_velocity_active_ = true;
    }
    esp_err_t status = ApplyConfiguration();
    if (status == ESP_OK) {
        status = ApplyPowerState();
    }
    if (status != ESP_OK) {
        acceleration_active_ = previous_acceleration;
        angular_velocity_active_ = previous_angular_velocity;
        configuration_ = previous_configuration;
        (void)ApplyConfiguration();
        (void)ApplyPowerState();
    }
    return status;
}

esp_err_t Icm42670::Suspend(Kind kind) {
    if (!available()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (kind == Kind::kAcceleration) {
        acceleration_active_ = false;
    } else {
        angular_velocity_active_ = false;
    }
    return ApplyPowerState();
}

esp_err_t Icm42670::Read(Kind kind, float (&values)[3]) {
    if (!available()) {
        return ESP_ERR_INVALID_STATE;
    }
    icm42670_value_t sample{};
    const esp_err_t status = kind == Kind::kAcceleration ? icm42670_get_acce_value(handle_, &sample)
                                                         : icm42670_get_gyro_value(handle_, &sample);
    if (status != ESP_OK) {
        return status;
    }
    const float scale = kind == Kind::kAcceleration ? kGravityMetersPerSecondSquared : kRadiansPerDegree;
    values[0] = sample.x * scale;
    values[1] = sample.y * scale;
    values[2] = sample.z * scale;
    return ESP_OK;
}

esp_err_t Icm42670::ApplyConfiguration() { return icm42670_config(handle_, &configuration_); }

esp_err_t Icm42670::ApplyPowerState() {
    const uint8_t acceleration = static_cast<uint8_t>(acceleration_active_ ? ACCE_PWR_LOWNOISE : ACCE_PWR_OFF);
    const uint8_t angular_velocity = static_cast<uint8_t>(angular_velocity_active_ ? GYRO_PWR_LOWNOISE : GYRO_PWR_OFF);
    const uint8_t value = static_cast<uint8_t>(acceleration | (angular_velocity << 2U));
    return icm42670_write_register(handle_, ICM42670_PWR_MGMT0, value);
}

icm42670_acce_odr_t Icm42670::AccelerationOdr(uint32_t interval_us) {
    if (interval_us <= 2500U) {
        return ACCE_ODR_400HZ;
    }
    if (interval_us <= 5000U) {
        return ACCE_ODR_200HZ;
    }
    if (interval_us <= 10000U) {
        return ACCE_ODR_100HZ;
    }
    if (interval_us <= 20000U) {
        return ACCE_ODR_50HZ;
    }
    if (interval_us <= 40000U) {
        return ACCE_ODR_25HZ;
    }
    return ACCE_ODR_12_5HZ;
}

icm42670_gyro_odr_t Icm42670::GyroscopeOdr(uint32_t interval_us) {
    if (interval_us <= 2500U) {
        return GYRO_ODR_400HZ;
    }
    if (interval_us <= 5000U) {
        return GYRO_ODR_200HZ;
    }
    if (interval_us <= 10000U) {
        return GYRO_ODR_100HZ;
    }
    if (interval_us <= 20000U) {
        return GYRO_ODR_50HZ;
    }
    return GYRO_ODR_25HZ;
}

esp_err_t Icm42670::Vector::Initialize(i2c_master_bus_handle_t bus) { return sensor_.Initialize(bus); }

esp_err_t Icm42670::Vector::Configure(uint32_t interval_us) { return sensor_.Configure(kind_, interval_us); }

esp_err_t Icm42670::Vector::Suspend() { return sensor_.Suspend(kind_); }

esp_err_t Icm42670::Vector::Read(float (&values)[3]) { return sensor_.Read(kind_, values); }

}  // namespace micropixel::platform::drivers
