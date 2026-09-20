#include "platform/boards/esp-mosaico/sensor_peripheral.hpp"

#include <cstdint>

#include "esp_err.h"
#include "esp_log.h"

namespace micropixel::platform::esp_mosaico {
namespace {

constexpr char kTag[] = "mosaico_sensors";

}  // namespace

SensorPeripheral::~SensorPeripheral() {
    if (initialization_started_ && !initialization_finished_) {
        (void)FinishInitialize();
    }
}

esp_err_t SensorPeripheral::BeginInitialize(i2c_master_bus_handle_t bus, buses::I2cExecutor& i2c_executor) {
    if (initialization_started_) {
        return ESP_ERR_INVALID_STATE;
    }
    bus_ = bus;
    i2c_executor_ = &i2c_executor;
    if (bus_ == nullptr) {
        ESP_LOGW(kTag, "shared I2C bus unavailable");
        return ESP_ERR_INVALID_ARG;
    }
    if (!i2c_executor.Post(
            buses::I2cExecutor::Priority::kNormal,
            [](void* context) {
                static_cast<SensorPeripheral*>(context)->InitializeOnWorker();
                return ESP_OK;
            },
            this)) {
        i2c_executor_ = nullptr;
        bus_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    initialization_started_ = true;
    return ESP_OK;
}

esp_err_t SensorPeripheral::FinishInitialize() {
    if (!initialization_started_ || initialization_finished_ || i2c_executor_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // Normal-priority jobs are FIFO. This barrier completes only after the
    // discovery job queued by BeginInitialize, while the main task is free to
    // initialize the display in parallel.
    const esp_err_t initialized = i2c_executor_->Invoke(
        buses::I2cExecutor::Priority::kNormal,
        [](void* context) {
            (void)context;
            return ESP_OK;
        },
        this);
    if (initialized != ESP_OK) {
        ESP_LOGW(kTag, "sensor discovery could not run on the shared I2C executor: %s", esp_err_to_name(initialized));
        return initialized;
    }
    const esp_err_t prepared = peripheral_.Initialize(*i2c_executor_);
    if (prepared != ESP_OK) {
        return prepared;
    }
    initialization_finished_ = true;
    return ESP_OK;
}

void SensorPeripheral::InitializeOnWorker() {
    if (const esp_err_t status = inertial_.Initialize(bus_); status != ESP_OK) {
        ESP_LOGW(kTag, "BMI270 unavailable: %s", esp_err_to_name(status));
    }
    if (const esp_err_t status = magnetic_field2_.Initialize(bus_); status != ESP_OK) {
        ESP_LOGW(kTag, "BMM150 #2 unavailable: %s", esp_err_to_name(status));
    }
    if (const esp_err_t status = magnetic_field3_.Initialize(bus_); status != ESP_OK) {
        ESP_LOGW(kTag, "BMM150 #3 unavailable: %s", esp_err_to_name(status));
    }
}

int32_t SensorPeripheral::GetInfo(device::PeripheralChannelId channel, micropixel_sensor_info_t& info_out) const {
    return peripheral_.GetInfo(channel, info_out);
}

int32_t SensorPeripheral::Start(device::PeripheralChannelId channel, uint32_t interval_us) {
    return peripheral_.Start(channel, interval_us);
}

int32_t SensorPeripheral::Read(device::PeripheralChannelId channel, device::SensorValues& values_out) {
    return peripheral_.Read(channel, values_out);
}

void SensorPeripheral::Stop(device::PeripheralChannelId channel) { peripheral_.Stop(channel); }

}  // namespace micropixel::platform::esp_mosaico
