#include "platform/boards/metalio-claw4/sensor_backend.hpp"

#include <cstdint>

#include "esp_err.h"
#include "esp_log.h"
#include "platform/boards/metalio-claw4/peripheral_ids.hpp"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "micropixel_sensors";
}  // namespace

SensorBackend::~SensorBackend() {
    Stop(peripheral_ids::kAcceleration);
    Stop(peripheral_ids::kMagneticField);
    if (acceleration_sampler_.timer != nullptr) {
        (void)esp_timer_delete(acceleration_sampler_.timer);
    }
    if (magnetic_field_sampler_.timer != nullptr) {
        (void)esp_timer_delete(magnetic_field_sampler_.timer);
    }
}

void SensorBackend::Initialize(i2c_master_bus_handle_t bus, common::I2cExecutor& i2c_executor) {
    bus_ = bus;
    i2c_executor_ = &i2c_executor;
    if (bus_ == nullptr) {
        ESP_LOGW(kTag, "shared I2C bus unavailable");
        return;
    }
    const esp_err_t initialized = i2c_executor.Invoke(
        common::I2cExecutor::Priority::kNormal,
        [](void* context) {
            static_cast<SensorBackend*>(context)->InitializeOnWorker();
            return ESP_OK;
        },
        this);
    if (initialized != ESP_OK) {
        ESP_LOGW(kTag, "sensor discovery could not run on the shared I2C executor: %s", esp_err_to_name(initialized));
        return;
    }

    const auto create_timer = [this](Sampler& sampler, micropixel_device_id_t device, const char* name) {
        sampler.owner = this;
        sampler.device = device;
        esp_timer_create_args_t arguments{};
        arguments.callback = TimerExpired;
        arguments.arg = &sampler;
        arguments.dispatch_method = ESP_TIMER_TASK;
        arguments.name = name;
        arguments.skip_unhandled_events = true;
        return esp_timer_create(&arguments, &sampler.timer);
    };
    if (acceleration_.available() &&
        create_timer(acceleration_sampler_, peripheral_ids::kAcceleration, "micropixel_accel") != ESP_OK) {
        ESP_LOGW(kTag, "acceleration timer unavailable");
    }
    if (magnetic_field_.available() &&
        create_timer(magnetic_field_sampler_, peripheral_ids::kMagneticField, "micropixel_magnet") != ESP_OK) {
        ESP_LOGW(kTag, "magnetometer timer unavailable");
    }
}

void SensorBackend::InitializeOnWorker() {
    if (const esp_err_t status = acceleration_.Initialize(bus_); status != ESP_OK) {
        ESP_LOGW(kTag, "SC7A20HTR unavailable: %s", esp_err_to_name(status));
    }
    if (const esp_err_t status = magnetic_field_.Initialize(bus_); status != ESP_OK) {
        ESP_LOGW(kTag, "QMC6309 unavailable: %s", esp_err_to_name(status));
    }
}

int32_t SensorBackend::GetInfo(micropixel_device_id_t device, micropixel_sensor_info_t& info_out) const {
    info_out = {};
    info_out.size = sizeof(info_out);
    info_out.device = device;
    info_out.placement = MICROPIXEL_SENSOR_PLACEMENT_BUILT_IN;
    info_out.value_count = 3U;
    if (device == peripheral_ids::kAcceleration && acceleration_.available()) {
        info_out.kind = MICROPIXEL_SENSOR_ACCELERATION;
        info_out.minimum_interval_us = acceleration_.minimum_interval_us();
        info_out.maximum_interval_us = 60000000U;
        return MICROPIXEL_STATUS_OK;
    }
    if (device == peripheral_ids::kMagneticField && magnetic_field_.available()) {
        info_out.kind = MICROPIXEL_SENSOR_MAGNETIC_FIELD;
        info_out.minimum_interval_us = magnetic_field_.minimum_interval_us();
        info_out.maximum_interval_us = 60000000U;
        return MICROPIXEL_STATUS_OK;
    }
    return MICROPIXEL_STATUS_NOT_FOUND;
}

int32_t SensorBackend::ConfigureOnWorker(micropixel_device_id_t device, uint32_t interval_us) {
    drivers::VectorSensor* driver = DriverFor(device);
    if (driver == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    return driver->Configure(interval_us) == ESP_OK ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
}

int32_t SensorBackend::Read(micropixel_device_id_t device, device::SensorValues& values_out) {
    values_out = {};
    const Sampler* sampler = FindSampler(device);
    if (sampler == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    portENTER_CRITICAL(&cache_lock_);
    const int32_t status = sampler->status;
    values_out = sampler->latest;
    portEXIT_CRITICAL(&cache_lock_);
    return status;
}

int32_t SensorBackend::Start(micropixel_device_id_t device, uint32_t interval_us) {
    Sampler* sampler = FindSampler(device);
    if (sampler == nullptr || sampler->timer == nullptr || i2c_executor_ == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    sampler->active.store(false, std::memory_order_release);
    (void)esp_timer_stop_blocking(sampler->timer, portMAX_DELAY);
    struct Request final {
        SensorBackend* backend;
        micropixel_device_id_t device;
        uint32_t interval_us;
        int32_t status;
    } request{this, device, interval_us, MICROPIXEL_STATUS_INTERNAL};
    const esp_err_t invoked = i2c_executor_->Invoke(
        common::I2cExecutor::Priority::kNormal,
        [](void* context) {
            auto& requested = *static_cast<Request*>(context);
            requested.status = requested.backend->ConfigureOnWorker(requested.device, requested.interval_us);
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
        Stop(device);
        return MICROPIXEL_STATUS_INTERNAL;
    }
    return MICROPIXEL_STATUS_OK;
}

void SensorBackend::StopOnWorker(micropixel_device_id_t device) {
    if (drivers::VectorSensor* driver = DriverFor(device); driver != nullptr) {
        (void)driver->Suspend();
    }
}

void SensorBackend::Stop(micropixel_device_id_t device) {
    Sampler* sampler = FindSampler(device);
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
            SensorBackend* backend;
            micropixel_device_id_t device;
        } request{this, device};
        (void)i2c_executor_->Invoke(
            common::I2cExecutor::Priority::kNormal,
            [](void* context) {
                auto& requested = *static_cast<Request*>(context);
                requested.backend->StopOnWorker(requested.device);
                return ESP_OK;
            },
            &request);
    }
}

SensorBackend::Sampler* SensorBackend::FindSampler(micropixel_device_id_t device) {
    if (device == peripheral_ids::kAcceleration && acceleration_.available()) {
        return &acceleration_sampler_;
    }
    if (device == peripheral_ids::kMagneticField && magnetic_field_.available()) {
        return &magnetic_field_sampler_;
    }
    return nullptr;
}

const SensorBackend::Sampler* SensorBackend::FindSampler(micropixel_device_id_t device) const {
    return const_cast<SensorBackend*>(this)->FindSampler(device);
}

void SensorBackend::TimerExpired(void* context) {
    auto* sampler = static_cast<Sampler*>(context);
    if (sampler == nullptr || sampler->owner == nullptr || !sampler->active.load(std::memory_order_acquire) ||
        sampler->pending.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!sampler->owner->i2c_executor_->Post(common::I2cExecutor::Priority::kLow, SampleOnWorker, sampler)) {
        sampler->pending.store(false, std::memory_order_release);
    }
}

esp_err_t SensorBackend::SampleOnWorker(void* context) {
    auto* sampler = static_cast<Sampler*>(context);
    if (sampler == nullptr || sampler->owner == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    sampler->pending.store(false, std::memory_order_release);
    if (!sampler->active.load(std::memory_order_acquire)) {
        return ESP_OK;
    }
    device::SensorValues values{};
    int32_t status = MICROPIXEL_STATUS_NOT_FOUND;
    if (sampler->device == peripheral_ids::kAcceleration) {
        status = sampler->owner->ReadAcceleration(values);
    } else if (sampler->device == peripheral_ids::kMagneticField) {
        status = sampler->owner->ReadMagneticField(values);
    }
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

drivers::VectorSensor* SensorBackend::DriverFor(micropixel_device_id_t device) {
    if (device == peripheral_ids::kAcceleration && acceleration_.available()) {
        return &acceleration_;
    }
    if (device == peripheral_ids::kMagneticField && magnetic_field_.available()) {
        return &magnetic_field_;
    }
    return nullptr;
}

int32_t SensorBackend::ReadAcceleration(device::SensorValues& values_out) {
    float values[3]{};
    if (acceleration_.Read(values) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis) {
        values_out.values[axis] = values[axis];
    }
    return MICROPIXEL_STATUS_OK;
}

int32_t SensorBackend::ReadMagneticField(device::SensorValues& values_out) {
    float values[3]{};
    if (magnetic_field_.Read(values) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis) {
        values_out.values[axis] = values[axis];
    }
    return MICROPIXEL_STATUS_OK;
}

}  // namespace micropixel::platform::metalio_claw4
