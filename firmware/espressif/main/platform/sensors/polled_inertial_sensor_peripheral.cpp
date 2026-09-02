#include "platform/sensors/polled_inertial_sensor_peripheral.hpp"

#include <array>
#include <cstdint>

#include "esp_err.h"
#include "esp_log.h"

namespace micropixel::platform::sensors {

PolledInertialSensorPeripheral::PolledInertialSensorPeripheral(drivers::VectorSensor& acceleration,
                                                               drivers::VectorSensor& angular_velocity,
                                                               PolledInertialSensorConfig config)
    : acceleration_(acceleration), angular_velocity_(angular_velocity), config_(config) {}

PolledInertialSensorPeripheral::~PolledInertialSensorPeripheral() {
    Stop(kAcceleration);
    Stop(kAngularVelocity);
    for (Sampler* sampler : std::array{&acceleration_sampler_, &angular_velocity_sampler_}) {
        if (sampler->timer != nullptr) {
            (void)esp_timer_delete(sampler->timer);
        }
    }
}

void PolledInertialSensorPeripheral::Initialize(i2c_master_bus_handle_t bus, buses::I2cExecutor& i2c_executor) {
    i2c_executor_ = &i2c_executor;
    if (bus == nullptr) {
        ESP_LOGW(config_.log_tag, "shared I2C bus unavailable");
        return;
    }
    struct Request final {
        PolledInertialSensorPeripheral* peripheral;
        i2c_master_bus_handle_t bus;
    } request{this, bus};
    const esp_err_t invoked = i2c_executor.Invoke(
        buses::I2cExecutor::Priority::kNormal,
        [](void* context) {
            auto& requested = *static_cast<Request*>(context);
            requested.peripheral->InitializeOnWorker(requested.bus);
            return ESP_OK;
        },
        &request);
    if (invoked != ESP_OK || (!acceleration_.available() && !angular_velocity_.available())) {
        ESP_LOGW(config_.log_tag, "%s unavailable", config_.model);
        return;
    }

    const auto create_timer = [this](Sampler& sampler, device::PeripheralChannelId channel, const char* name) {
        sampler.owner = this;
        sampler.channel = channel;
        esp_timer_create_args_t arguments{};
        arguments.callback = TimerExpired;
        arguments.arg = &sampler;
        arguments.dispatch_method = ESP_TIMER_TASK;
        arguments.name = name;
        arguments.skip_unhandled_events = true;
        return esp_timer_create(&arguments, &sampler.timer);
    };
    if (acceleration_.available() &&
        create_timer(acceleration_sampler_, kAcceleration, config_.acceleration_timer_name) != ESP_OK) {
        ESP_LOGW(config_.log_tag, "accelerometer timer unavailable");
    }
    if (angular_velocity_.available() &&
        create_timer(angular_velocity_sampler_, kAngularVelocity, config_.angular_velocity_timer_name) != ESP_OK) {
        ESP_LOGW(config_.log_tag, "gyroscope timer unavailable");
    }
}

void PolledInertialSensorPeripheral::InitializeOnWorker(i2c_master_bus_handle_t bus) {
    if (const esp_err_t status = acceleration_.Initialize(bus); status != ESP_OK) {
        ESP_LOGW(config_.log_tag, "%s initialization failed: %s", config_.model, esp_err_to_name(status));
    }
}

int32_t PolledInertialSensorPeripheral::GetInfo(device::PeripheralChannelId channel,
                                                micropixel_sensor_info_t& info_out) const {
    info_out = {};
    info_out.size = sizeof(info_out);
    info_out.placement = MICROPIXEL_SENSOR_PLACEMENT_BUILT_IN;
    info_out.value_count = 3U;
    const drivers::VectorSensor* driver = const_cast<PolledInertialSensorPeripheral*>(this)->DriverFor(channel);
    if (driver == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    info_out.kind = channel == kAcceleration ? MICROPIXEL_SENSOR_ACCELERATION : MICROPIXEL_SENSOR_ANGULAR_VELOCITY;
    info_out.minimum_interval_us = driver->minimum_interval_us();
    info_out.maximum_interval_us = 60000000U;
    return MICROPIXEL_STATUS_OK;
}

int32_t PolledInertialSensorPeripheral::Start(device::PeripheralChannelId channel, uint32_t interval_us) {
    Sampler* sampler = FindSampler(channel);
    if (sampler == nullptr || sampler->timer == nullptr || i2c_executor_ == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    sampler->active.store(false, std::memory_order_release);
    (void)esp_timer_stop_blocking(sampler->timer, portMAX_DELAY);
    struct Request final {
        PolledInertialSensorPeripheral* peripheral;
        device::PeripheralChannelId channel;
        uint32_t interval_us;
        int32_t status;
    } request{this, channel, interval_us, MICROPIXEL_STATUS_INTERNAL};
    const esp_err_t invoked = i2c_executor_->Invoke(
        buses::I2cExecutor::Priority::kNormal,
        [](void* context) {
            auto& requested = *static_cast<Request*>(context);
            requested.status = requested.peripheral->ConfigureOnWorker(requested.channel, requested.interval_us);
            return requested.status == MICROPIXEL_STATUS_OK ? ESP_OK : ESP_FAIL;
        },
        &request);
    if (invoked != ESP_OK || request.status != MICROPIXEL_STATUS_OK) {
        return request.status;
    }
    portENTER_CRITICAL(&cache_lock_);
    sampler->latest = {};
    sampler->status = MICROPIXEL_STATUS_WOULD_BLOCK;
    portEXIT_CRITICAL(&cache_lock_);
    sampler->active.store(true, std::memory_order_release);
    if (esp_timer_start_periodic(sampler->timer, interval_us) != ESP_OK) {
        sampler->active.store(false, std::memory_order_release);
        Stop(channel);
        return MICROPIXEL_STATUS_INTERNAL;
    }
    return MICROPIXEL_STATUS_OK;
}

int32_t PolledInertialSensorPeripheral::Read(device::PeripheralChannelId channel, device::SensorValues& values_out) {
    values_out = {};
    const Sampler* sampler = FindSampler(channel);
    if (sampler == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    portENTER_CRITICAL(&cache_lock_);
    const int32_t status = sampler->status;
    values_out = sampler->latest;
    portEXIT_CRITICAL(&cache_lock_);
    return status;
}

void PolledInertialSensorPeripheral::Stop(device::PeripheralChannelId channel) {
    Sampler* sampler = FindSampler(channel);
    if (sampler == nullptr) {
        return;
    }
    sampler->active.store(false, std::memory_order_release);
    if (sampler->timer != nullptr) {
        (void)esp_timer_stop_blocking(sampler->timer, portMAX_DELAY);
    }
    portENTER_CRITICAL(&cache_lock_);
    sampler->latest = {};
    sampler->status = MICROPIXEL_STATUS_WOULD_BLOCK;
    portEXIT_CRITICAL(&cache_lock_);
    if (i2c_executor_ != nullptr) {
        struct Request final {
            PolledInertialSensorPeripheral* peripheral;
            device::PeripheralChannelId channel;
        } request{this, channel};
        (void)i2c_executor_->Invoke(
            buses::I2cExecutor::Priority::kNormal,
            [](void* context) {
                auto& requested = *static_cast<Request*>(context);
                requested.peripheral->StopOnWorker(requested.channel);
                return ESP_OK;
            },
            &request);
    }
}

int32_t PolledInertialSensorPeripheral::ConfigureOnWorker(device::PeripheralChannelId channel, uint32_t interval_us) {
    drivers::VectorSensor* driver = DriverFor(channel);
    if (driver == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    return driver->Configure(interval_us) == ESP_OK ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
}

void PolledInertialSensorPeripheral::StopOnWorker(device::PeripheralChannelId channel) {
    if (drivers::VectorSensor* driver = DriverFor(channel); driver != nullptr) {
        (void)driver->Suspend();
    }
}

drivers::VectorSensor* PolledInertialSensorPeripheral::DriverFor(device::PeripheralChannelId channel) {
    if (channel == kAcceleration && acceleration_.available()) {
        return &acceleration_;
    }
    if (channel == kAngularVelocity && angular_velocity_.available()) {
        return &angular_velocity_;
    }
    return nullptr;
}

PolledInertialSensorPeripheral::Sampler* PolledInertialSensorPeripheral::FindSampler(
    device::PeripheralChannelId channel) {
    if (channel == kAcceleration && acceleration_.available()) {
        return &acceleration_sampler_;
    }
    if (channel == kAngularVelocity && angular_velocity_.available()) {
        return &angular_velocity_sampler_;
    }
    return nullptr;
}

const PolledInertialSensorPeripheral::Sampler* PolledInertialSensorPeripheral::FindSampler(
    device::PeripheralChannelId channel) const {
    return const_cast<PolledInertialSensorPeripheral*>(this)->FindSampler(channel);
}

void PolledInertialSensorPeripheral::TimerExpired(void* context) {
    auto* sampler = static_cast<Sampler*>(context);
    if (sampler == nullptr || sampler->owner == nullptr || !sampler->active.load(std::memory_order_acquire) ||
        sampler->pending.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!sampler->owner->i2c_executor_->Post(buses::I2cExecutor::Priority::kLow, SampleOnWorker, sampler)) {
        sampler->pending.store(false, std::memory_order_release);
    }
}

esp_err_t PolledInertialSensorPeripheral::SampleOnWorker(void* context) {
    auto* sampler = static_cast<Sampler*>(context);
    if (sampler == nullptr || sampler->owner == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    sampler->pending.store(false, std::memory_order_release);
    if (!sampler->active.load(std::memory_order_acquire)) {
        return ESP_OK;
    }
    device::SensorValues values{};
    const int32_t status = sampler->owner->ReadVector(sampler->channel, values);
    values.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
    if (sampler->active.load(std::memory_order_acquire)) {
        portENTER_CRITICAL(&sampler->owner->cache_lock_);
        if (status == MICROPIXEL_STATUS_OK) {
            sampler->latest = values;
        }
        sampler->status = status;
        portEXIT_CRITICAL(&sampler->owner->cache_lock_);
    }
    return status == MICROPIXEL_STATUS_OK ? ESP_OK : ESP_FAIL;
}

int32_t PolledInertialSensorPeripheral::ReadVector(device::PeripheralChannelId channel,
                                                   device::SensorValues& values_out) {
    drivers::VectorSensor* driver = DriverFor(channel);
    if (driver == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    float values[3]{};
    if (driver->Read(values) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis) {
        values_out.values[axis] = values[axis];
    }
    return MICROPIXEL_STATUS_OK;
}

}  // namespace micropixel::platform::sensors
