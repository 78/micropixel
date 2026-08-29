#include "platform/drivers/sensors/bmi270.hpp"

#include <array>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

namespace micropixel::platform::drivers {
namespace {

constexpr char kTag[] = "bmi270";
constexpr uint8_t kAddress = 0x69U;
constexpr uint32_t kI2cSpeedHz = 400000U;
constexpr int kTimeoutMs = 100;
constexpr uint16_t kMaximumTransferBytes = 32U;
constexpr float kGravityMetersPerSecondSquared = 9.80665F;
constexpr float kRadiansPerDegree = 0.017453292519943295F;

esp_err_t ApiStatus(int8_t status) { return status == BMI2_OK ? ESP_OK : ESP_FAIL; }

}  // namespace

Bmi270::~Bmi270() {
    if (device_handle_ != nullptr) {
        (void)i2c_master_bus_rm_device(device_handle_);
    }
}

esp_err_t Bmi270::Initialize(i2c_master_bus_handle_t bus) {
    if (bus == nullptr || i2c_master_probe(bus, kAddress, kTimeoutMs) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = kAddress;
    config.scl_speed_hz = kI2cSpeedHz;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &device_handle_), kTag, "add I2C device failed");

    device_ = {};
    device_.intf = BMI2_I2C_INTF;
    device_.intf_ptr = this;
    device_.read = ReadRegisters;
    device_.write = WriteRegisters;
    device_.delay_us = DelayUs;
    device_.read_write_len = kMaximumTransferBytes;
    const int8_t status = bmi270_init(&device_);
    if (status != BMI2_OK || device_.chip_id != BMI270_CHIP_ID) {
        ESP_LOGW(kTag, "initialization failed: status=%d chip=0x%02x", static_cast<int>(status), device_.chip_id);
        (void)i2c_master_bus_rm_device(device_handle_);
        device_handle_ = nullptr;
        return status == BMI2_OK ? ESP_ERR_INVALID_RESPONSE : ESP_FAIL;
    }
    const uint8_t sensors[] = {BMI2_ACCEL, BMI2_GYRO};
    const esp_err_t suspended = ApiStatus(bmi2_sensor_disable(sensors, sizeof(sensors), &device_));
    if (suspended != ESP_OK) {
        ESP_LOGW(kTag, "initial suspend failed");
        (void)i2c_master_bus_rm_device(device_handle_);
        device_handle_ = nullptr;
        return suspended;
    }
    ESP_LOGI(kTag, "ready: CHIP_ID=0x%02x", device_.chip_id);
    return ESP_OK;
}

esp_err_t Bmi270::Configure(Kind kind, uint32_t interval_us) {
    if (!available()) {
        return ESP_ERR_INVALID_STATE;
    }
    bmi2_sens_config config{};
    uint8_t sensor = BMI2_ACCEL;
    if (kind == Kind::kAcceleration) {
        config.type = BMI2_ACCEL;
        ESP_RETURN_ON_ERROR(ApiStatus(bmi2_get_sensor_config(&config, 1U, &device_)), kTag,
                            "read accelerometer configuration failed");
        config.cfg.acc.odr = AccelerationOdr(interval_us);
        config.cfg.acc.range = BMI2_ACC_RANGE_2G;
        config.cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
        config.cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
    } else {
        sensor = BMI2_GYRO;
        config.type = BMI2_GYRO;
        ESP_RETURN_ON_ERROR(ApiStatus(bmi2_get_sensor_config(&config, 1U, &device_)), kTag,
                            "read gyroscope configuration failed");
        config.cfg.gyr.odr = GyroscopeOdr(interval_us);
        config.cfg.gyr.range = BMI2_GYR_RANGE_2000;
        config.cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
        config.cfg.gyr.noise_perf = BMI2_POWER_OPT_MODE;
        config.cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;
    }
    ESP_RETURN_ON_ERROR(ApiStatus(bmi2_set_sensor_config(&config, 1U, &device_)), kTag,
                        "write sensor configuration failed");
    return ApiStatus(bmi2_sensor_enable(&sensor, 1U, &device_));
}

esp_err_t Bmi270::Suspend(Kind kind) {
    if (!available()) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t sensor = kind == Kind::kAcceleration ? BMI2_ACCEL : BMI2_GYRO;
    return ApiStatus(bmi2_sensor_disable(&sensor, 1U, &device_));
}

esp_err_t Bmi270::Read(Kind kind, float (&values)[3]) {
    if (!available()) {
        return ESP_ERR_INVALID_STATE;
    }
    bmi2_sens_data data{};
    ESP_RETURN_ON_ERROR(ApiStatus(bmi2_get_sensor_data(&data, &device_)), kTag, "read sensor data failed");
    const bmi2_sens_axes_data& axes = kind == Kind::kAcceleration ? data.acc : data.gyr;
    constexpr float kHalfScale = 32768.0F;
    const float scale = kind == Kind::kAcceleration ? kGravityMetersPerSecondSquared * 2.0F / kHalfScale
                                                    : 2000.0F * kRadiansPerDegree / kHalfScale;
    values[0] = static_cast<float>(axes.x) * scale;
    values[1] = static_cast<float>(axes.y) * scale;
    values[2] = static_cast<float>(axes.z) * scale;
    return ESP_OK;
}

int8_t Bmi270::ReadRegisters(uint8_t register_address, uint8_t* data, uint32_t length, void* context) {
    auto* sensor = static_cast<Bmi270*>(context);
    if (sensor == nullptr || sensor->device_handle_ == nullptr || data == nullptr || length == 0U) {
        return -1;
    }
    return i2c_master_transmit_receive(sensor->device_handle_, &register_address, sizeof(register_address), data,
                                       length, kTimeoutMs) == ESP_OK
               ? BMI2_INTF_RET_SUCCESS
               : -1;
}

int8_t Bmi270::WriteRegisters(uint8_t register_address, const uint8_t* data, uint32_t length, void* context) {
    auto* sensor = static_cast<Bmi270*>(context);
    if (sensor == nullptr || sensor->device_handle_ == nullptr || data == nullptr || length == 0U ||
        length > kMaximumTransferBytes) {
        return -1;
    }
    std::array<uint8_t, kMaximumTransferBytes + 1U> transaction{};
    transaction[0] = register_address;
    for (uint32_t index = 0U; index < length; ++index) {
        transaction[index + 1U] = data[index];
    }
    return i2c_master_transmit(sensor->device_handle_, transaction.data(), length + 1U, kTimeoutMs) == ESP_OK
               ? BMI2_INTF_RET_SUCCESS
               : -1;
}

void Bmi270::DelayUs(uint32_t period_us, void*) { esp_rom_delay_us(period_us); }

uint8_t Bmi270::AccelerationOdr(uint32_t interval_us) {
    if (interval_us <= 2500U) {
        return BMI2_ACC_ODR_400HZ;
    }
    if (interval_us <= 5000U) {
        return BMI2_ACC_ODR_200HZ;
    }
    if (interval_us <= 10000U) {
        return BMI2_ACC_ODR_100HZ;
    }
    if (interval_us <= 20000U) {
        return BMI2_ACC_ODR_50HZ;
    }
    return interval_us <= 40000U ? BMI2_ACC_ODR_25HZ : BMI2_ACC_ODR_12_5HZ;
}

uint8_t Bmi270::GyroscopeOdr(uint32_t interval_us) {
    if (interval_us <= 2500U) {
        return BMI2_GYR_ODR_400HZ;
    }
    if (interval_us <= 5000U) {
        return BMI2_GYR_ODR_200HZ;
    }
    if (interval_us <= 10000U) {
        return BMI2_GYR_ODR_100HZ;
    }
    return interval_us <= 20000U ? BMI2_GYR_ODR_50HZ : BMI2_GYR_ODR_25HZ;
}

}  // namespace micropixel::platform::drivers
