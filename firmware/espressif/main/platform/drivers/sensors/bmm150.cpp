#include "platform/drivers/sensors/bmm150.hpp"

#include <array>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

namespace micropixel::platform::drivers {
namespace {

constexpr char kTag[] = "bmm150";
constexpr uint32_t kI2cSpeedHz = 400000U;
constexpr int kTimeoutMs = 100;
constexpr uint32_t kMaximumWriteBytes = 8U;

esp_err_t ApiStatus(int8_t status) { return status == BMM150_OK ? ESP_OK : ESP_FAIL; }

}  // namespace

Bmm150::~Bmm150() {
    if (device_handle_ != nullptr) {
        (void)i2c_master_bus_rm_device(device_handle_);
    }
}

esp_err_t Bmm150::Initialize(i2c_master_bus_handle_t bus) {
    if (bus == nullptr || i2c_master_probe(bus, address_, kTimeoutMs) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = address_;
    config.scl_speed_hz = kI2cSpeedHz;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &device_handle_), kTag, "add I2C device failed");

    device_ = {};
    device_.intf = BMM150_I2C_INTF;
    device_.intf_ptr = this;
    device_.read = ReadRegisters;
    device_.write = WriteRegisters;
    device_.delay_us = DelayUs;
    const int8_t status = bmm150_init(&device_);
    if (status != BMM150_OK || device_.chip_id != BMM150_CHIP_ID) {
        ESP_LOGW(kTag, "0x%02x initialization failed: status=%d chip=0x%02x", address_, static_cast<int>(status),
                 device_.chip_id);
        (void)i2c_master_bus_rm_device(device_handle_);
        device_handle_ = nullptr;
        return status == BMM150_OK ? ESP_ERR_INVALID_RESPONSE : ESP_FAIL;
    }
    const esp_err_t suspended = Suspend();
    if (suspended != ESP_OK) {
        ESP_LOGW(kTag, "0x%02x initial suspend failed", address_);
        (void)i2c_master_bus_rm_device(device_handle_);
        device_handle_ = nullptr;
        return suspended;
    }
    ESP_LOGI(kTag, "ready: address=0x%02x CHIP_ID=0x%02x", address_, device_.chip_id);
    return ESP_OK;
}

esp_err_t Bmm150::Configure(uint32_t interval_us) {
    if (!available()) {
        return ESP_ERR_INVALID_STATE;
    }
    bmm150_settings settings{};
    settings.preset_mode = BMM150_PRESETMODE_REGULAR;
    ESP_RETURN_ON_ERROR(ApiStatus(bmm150_set_presetmode(&settings, &device_)), kTag, "set preset failed");
    settings.data_rate = DataRate(interval_us);
    ESP_RETURN_ON_ERROR(ApiStatus(bmm150_set_sensor_settings(BMM150_SEL_DATA_RATE, &settings, &device_)), kTag,
                        "set data rate failed");
    settings.pwr_mode = BMM150_POWERMODE_NORMAL;
    return ApiStatus(bmm150_set_op_mode(&settings, &device_));
}

esp_err_t Bmm150::Suspend() {
    if (!available()) {
        return ESP_ERR_INVALID_STATE;
    }
    bmm150_settings settings{};
    settings.pwr_mode = BMM150_POWERMODE_SUSPEND;
    return ApiStatus(bmm150_set_op_mode(&settings, &device_));
}

esp_err_t Bmm150::Read(float (&values)[3]) {
    if (!available()) {
        return ESP_ERR_INVALID_STATE;
    }
    bmm150_mag_data data{};
    ESP_RETURN_ON_ERROR(ApiStatus(bmm150_read_mag_data(&data, &device_)), kTag, "read magnetic field failed");
    values[0] = static_cast<float>(data.x);
    values[1] = static_cast<float>(data.y);
    values[2] = static_cast<float>(data.z);
    return ESP_OK;
}

int8_t Bmm150::ReadRegisters(uint8_t register_address, uint8_t* data, uint32_t length, void* context) {
    auto* sensor = static_cast<Bmm150*>(context);
    if (sensor == nullptr || sensor->device_handle_ == nullptr || data == nullptr || length == 0U) {
        return -1;
    }
    return i2c_master_transmit_receive(sensor->device_handle_, &register_address, sizeof(register_address), data,
                                       length, kTimeoutMs) == ESP_OK
               ? BMM150_INTF_RET_SUCCESS
               : -1;
}

int8_t Bmm150::WriteRegisters(uint8_t register_address, const uint8_t* data, uint32_t length, void* context) {
    auto* sensor = static_cast<Bmm150*>(context);
    if (sensor == nullptr || sensor->device_handle_ == nullptr || data == nullptr || length == 0U ||
        length > kMaximumWriteBytes) {
        return -1;
    }
    std::array<uint8_t, kMaximumWriteBytes + 1U> transaction{};
    transaction[0] = register_address;
    for (uint32_t index = 0U; index < length; ++index) {
        transaction[index + 1U] = data[index];
    }
    return i2c_master_transmit(sensor->device_handle_, transaction.data(), length + 1U, kTimeoutMs) == ESP_OK
               ? BMM150_INTF_RET_SUCCESS
               : -1;
}

void Bmm150::DelayUs(uint32_t period_us, void*) { esp_rom_delay_us(period_us); }

uint8_t Bmm150::DataRate(uint32_t interval_us) {
    if (interval_us <= 33334U) {
        return BMM150_DATA_RATE_30HZ;
    }
    if (interval_us <= 40000U) {
        return BMM150_DATA_RATE_25HZ;
    }
    if (interval_us <= 50000U) {
        return BMM150_DATA_RATE_20HZ;
    }
    if (interval_us <= 66667U) {
        return BMM150_DATA_RATE_15HZ;
    }
    if (interval_us <= 100000U) {
        return BMM150_DATA_RATE_10HZ;
    }
    if (interval_us <= 125000U) {
        return BMM150_DATA_RATE_08HZ;
    }
    return interval_us <= 166667U ? BMM150_DATA_RATE_06HZ : BMM150_DATA_RATE_02HZ;
}

}  // namespace micropixel::platform::drivers
