#include "platform/boards/esp-mosaico/sensor_peripheral.hpp"

#include <array>
#include <cstdint>

#include "esp_err.h"
#include "esp_log.h"

namespace micropixel::platform::esp_mosaico {
namespace {

constexpr char kTag[] = "mosaico_sensors";

}  // namespace

SensorPeripheral::~SensorPeripheral() {
    Stop(kAcceleration);
    Stop(kAngularVelocity);
    Stop(kMagneticField2);
    Stop(kMagneticField3);
    for (Sampler* sampler : std::array{&acceleration_sampler_, &angular_velocity_sampler_, &magnetic_field2_sampler_,
                                       &magnetic_field3_sampler_}) {
        if (sampler->timer != nullptr) {
            (void)esp_timer_delete(sampler->timer);
        }
    }
}

void SensorPeripheral::Initialize(i2c_master_bus_handle_t bus, buses::I2cExecutor& i2c_executor) {
    bus_ = bus;
    i2c_executor_ = &i2c_executor;
    if (bus_ == nullptr) {
        ESP_LOGW(kTag, "shared I2C bus unavailable");
        return;
    }
    const esp_err_t initialized = i2c_executor.Invoke(
        buses::I2cExecutor::Priority::kNormal,
        [](void* context) {
            static_cast<SensorPeripheral*>(context)->InitializeOnWorker();
            return ESP_OK;
        },
        this);
    if (initialized != ESP_OK) {
        ESP_LOGW(kTag, "sensor discovery could not run on the shared I2C executor: %s", esp_err_to_name(initialized));
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
    const auto prepare = [&](drivers::VectorSensor& driver, Sampler& sampler, device::PeripheralChannelId channel,
                             const char* name) {
        if (driver.available() && create_timer(sampler, channel, name) != ESP_OK) {
            ESP_LOGW(kTag, "%s timer unavailable", name);
        }
    };
    prepare(acceleration_, acceleration_sampler_, kAcceleration, "mosaico_accel");
    prepare(angular_velocity_, angular_velocity_sampler_, kAngularVelocity, "mosaico_gyro");
    prepare(magnetic_field2_, magnetic_field2_sampler_, kMagneticField2, "mosaico_mag2");
    prepare(magnetic_field3_, magnetic_field3_sampler_, kMagneticField3, "mosaico_mag3");
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
    info_out = {};
    info_out.size = sizeof(info_out);
    info_out.placement = MICROPIXEL_SENSOR_PLACEMENT_BUILT_IN;
    info_out.value_count = 3U;
    const drivers::VectorSensor* driver = const_cast<SensorPeripheral*>(this)->DriverFor(channel);
    if (driver == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (channel == kAcceleration) {
        info_out.kind = MICROPIXEL_SENSOR_ACCELERATION;
    } else if (channel == kAngularVelocity) {
        info_out.kind = MICROPIXEL_SENSOR_ANGULAR_VELOCITY;
    } else {
        info_out.kind = MICROPIXEL_SENSOR_MAGNETIC_FIELD;
    }
    info_out.minimum_interval_us = driver->minimum_interval_us();
    info_out.maximum_interval_us = 60000000U;
    return MICROPIXEL_STATUS_OK;
}

int32_t SensorPeripheral::ConfigureOnWorker(device::PeripheralChannelId channel, uint32_t interval_us) {
    drivers::VectorSensor* driver = DriverFor(channel);
    if (driver == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    return driver->Configure(interval_us) == ESP_OK ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
}

int32_t SensorPeripheral::Read(device::PeripheralChannelId channel, device::SensorValues& values_out) {
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

int32_t SensorPeripheral::Start(device::PeripheralChannelId channel, uint32_t interval_us) {
    Sampler* sampler = FindSampler(channel);
    if (sampler == nullptr || sampler->timer == nullptr || i2c_executor_ == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    sampler->active.store(false, std::memory_order_release);
    (void)esp_timer_stop_blocking(sampler->timer, portMAX_DELAY);
    struct Request final {
        SensorPeripheral* peripheral;
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

void SensorPeripheral::StopOnWorker(device::PeripheralChannelId channel) {
    if (drivers::VectorSensor* driver = DriverFor(channel); driver != nullptr) {
        (void)driver->Suspend();
    }
}

void SensorPeripheral::Stop(device::PeripheralChannelId channel) {
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
            SensorPeripheral* peripheral;
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

SensorPeripheral::Sampler* SensorPeripheral::FindSampler(device::PeripheralChannelId channel) {
    if (channel == kAcceleration && acceleration_.available()) {
        return &acceleration_sampler_;
    }
    if (channel == kAngularVelocity && angular_velocity_.available()) {
        return &angular_velocity_sampler_;
    }
    if (channel == kMagneticField2 && magnetic_field2_.available()) {
        return &magnetic_field2_sampler_;
    }
    if (channel == kMagneticField3 && magnetic_field3_.available()) {
        return &magnetic_field3_sampler_;
    }
    return nullptr;
}

const SensorPeripheral::Sampler* SensorPeripheral::FindSampler(device::PeripheralChannelId channel) const {
    return const_cast<SensorPeripheral*>(this)->FindSampler(channel);
}

void SensorPeripheral::TimerExpired(void* context) {
    auto* sampler = static_cast<Sampler*>(context);
    if (sampler == nullptr || sampler->owner == nullptr || !sampler->active.load(std::memory_order_acquire) ||
        sampler->pending.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!sampler->owner->i2c_executor_->Post(buses::I2cExecutor::Priority::kLow, SampleOnWorker, sampler)) {
        sampler->pending.store(false, std::memory_order_release);
    }
}

esp_err_t SensorPeripheral::SampleOnWorker(void* context) {
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

drivers::VectorSensor* SensorPeripheral::DriverFor(device::PeripheralChannelId channel) {
    if (channel == kAcceleration && acceleration_.available()) {
        return &acceleration_;
    }
    if (channel == kAngularVelocity && angular_velocity_.available()) {
        return &angular_velocity_;
    }
    if (channel == kMagneticField2 && magnetic_field2_.available()) {
        return &magnetic_field2_;
    }
    if (channel == kMagneticField3 && magnetic_field3_.available()) {
        return &magnetic_field3_;
    }
    return nullptr;
}

int32_t SensorPeripheral::ReadVector(device::PeripheralChannelId channel, device::SensorValues& values_out) {
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

}  // namespace micropixel::platform::esp_mosaico
