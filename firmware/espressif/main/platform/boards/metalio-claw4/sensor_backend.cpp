#include "platform/boards/metalio-claw4/sensor_backend.hpp"

#include <cstdint>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/boards/metalio-claw4/peripheral_ids.hpp"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "micropixel_sensors";
constexpr uint32_t kI2cSpeedHz = 100000U;
constexpr int kI2cTimeoutMs = 50;

constexpr uint8_t kSc7Address = 0x19U;
constexpr uint8_t kSc7WhoAmIRegister = 0x0fU;
constexpr uint8_t kSc7Control1Register = 0x20U;
constexpr uint8_t kSc7Control4Register = 0x23U;
constexpr uint8_t kSc7OutputXLowRegister = 0x28U;
constexpr uint8_t kSc7AutoIncrement = 0x80U;
constexpr uint8_t kSc7WhoAmI = 0x11U;
constexpr uint8_t kSc7PowerDown = 0x07U;
constexpr uint8_t kSc7Sample10Hz = 0x27U;
constexpr uint8_t kSc7Sample50Hz = 0x47U;
constexpr uint8_t kSc7Sample100Hz = 0x57U;
constexpr uint8_t kSc7Sample200Hz = 0x67U;
constexpr uint8_t kSc7Sample400Hz = 0x77U;
constexpr uint8_t kSc7Control4Value = 0x88U;
constexpr float kMetersPerSecondSquaredPerMilliG = 0.00980665F;

constexpr uint8_t kQmcAddress = 0x7cU;
constexpr uint8_t kQmcChipIdRegister = 0x00U;
constexpr uint8_t kQmcOutputXLowRegister = 0x01U;
constexpr uint8_t kQmcControl1Register = 0x0aU;
constexpr uint8_t kQmcControl2Register = 0x0bU;
constexpr uint8_t kQmcChipId = 0x90U;
// Lowest OSR1/OSR2 power settings, with 10 Hz ODR and the +/-32 gauss range.
constexpr uint8_t kQmcLowPowerOsr = 0x18U;
constexpr uint8_t kQmcControl2Normal10Hz = 0x10U;
constexpr uint8_t kQmcControl2Normal50Hz = 0x20U;
constexpr uint8_t kQmcControl2Normal100Hz = 0x30U;
constexpr uint8_t kQmcControl2Normal200Hz = 0x40U;
constexpr uint8_t kQmcSoftReset = 0x80U;
constexpr uint8_t kQmcSuspendMode = 0x00U;
constexpr uint8_t kQmcNormalMode = 0x01U;
// RNG=00 selects the documented +/-32 gauss range at
// 1000 LSB/gauss. 1 gauss is 100 microtesla.
constexpr float kMicroteslaPerLsb = 0.1F;

int16_t Signed16(uint8_t low, uint8_t high) { return static_cast<int16_t>((static_cast<uint16_t>(high) << 8U) | low); }

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
    const auto remove_device = [&i2c_executor](i2c_master_dev_handle_t& device) {
        i2c_master_dev_handle_t removed = device;
        (void)i2c_executor.Invoke(
            common::I2cExecutor::Priority::kNormal,
            [](void* context) { return i2c_master_bus_rm_device(*static_cast<i2c_master_dev_handle_t*>(context)); },
            &removed);
        device = nullptr;
    };
    if (acceleration_ != nullptr &&
        create_timer(acceleration_sampler_, peripheral_ids::kAcceleration, "micropixel_accel") != ESP_OK) {
        remove_device(acceleration_);
    }
    if (magnetic_field_ != nullptr &&
        create_timer(magnetic_field_sampler_, peripheral_ids::kMagneticField, "micropixel_magnet") != ESP_OK) {
        remove_device(magnetic_field_);
    }
}

void SensorBackend::InitializeOnWorker() {
    if (i2c_master_probe(bus_, kSc7Address, kI2cTimeoutMs) == ESP_OK && AddDevice(kSc7Address, acceleration_)) {
        uint8_t who_am_i = 0U;
        if (!ReadRegister(acceleration_, kSc7WhoAmIRegister, who_am_i) || who_am_i != kSc7WhoAmI ||
            !WriteRegister(acceleration_, kSc7Control4Register, kSc7Control4Value) ||
            !WriteRegister(acceleration_, kSc7Control1Register, kSc7PowerDown)) {
            (void)i2c_master_bus_rm_device(acceleration_);
            acceleration_ = nullptr;
        } else {
            ESP_LOGI(kTag, "SC7A20HTR ready: WHO_AM_I=0x%02x", who_am_i);
        }
    }
    if (acceleration_ == nullptr) {
        ESP_LOGW(kTag, "SC7A20HTR unavailable at I2C address 0x%02x", kSc7Address);
    }

    if (i2c_master_probe(bus_, kQmcAddress, kI2cTimeoutMs) == ESP_OK && AddDevice(kQmcAddress, magnetic_field_)) {
        uint8_t chip_id = 0U;
        bool configured = ReadRegister(magnetic_field_, kQmcChipIdRegister, chip_id) && chip_id == kQmcChipId;
        configured = configured && WriteRegister(magnetic_field_, kQmcControl2Register, kQmcSoftReset);
        vTaskDelay(pdMS_TO_TICKS(25U));
        configured = configured && WriteRegister(magnetic_field_, kQmcControl2Register, 0U);
        vTaskDelay(pdMS_TO_TICKS(25U));
        configured = configured && WriteRegister(magnetic_field_, kQmcControl1Register, kQmcSuspendMode);
        if (!configured) {
            ESP_LOGW(kTag, "QMC6309 rejected configuration: CHIP_ID=0x%02x", chip_id);
            (void)i2c_master_bus_rm_device(magnetic_field_);
            magnetic_field_ = nullptr;
        } else {
            ESP_LOGI(kTag, "QMC6309 ready in suspend mode: CHIP_ID=0x%02x", chip_id);
        }
    }
    if (magnetic_field_ == nullptr) {
        ESP_LOGW(kTag, "QMC6309 unavailable at I2C address 0x%02x", kQmcAddress);
    }
}

bool SensorBackend::AddDevice(uint8_t address, i2c_master_dev_handle_t& device_out) {
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = address;
    config.scl_speed_hz = kI2cSpeedHz;
    return i2c_master_bus_add_device(bus_, &config, &device_out) == ESP_OK;
}

bool SensorBackend::ReadRegister(i2c_master_dev_handle_t device, uint8_t address, uint8_t& value_out) const {
    return ReadRegisters(device, address, &value_out, sizeof(value_out));
}

bool SensorBackend::WriteRegister(i2c_master_dev_handle_t device, uint8_t address, uint8_t value) const {
    const uint8_t transaction[] = {address, value};
    return device != nullptr && i2c_master_transmit(device, transaction, sizeof(transaction), kI2cTimeoutMs) == ESP_OK;
}

bool SensorBackend::ReadRegisters(i2c_master_dev_handle_t device, uint8_t address, uint8_t* bytes,
                                  size_t length) const {
    return device != nullptr && bytes != nullptr && length != 0U &&
           i2c_master_transmit_receive(device, &address, sizeof(address), bytes, length, kI2cTimeoutMs) == ESP_OK;
}

int32_t SensorBackend::GetInfo(micropixel_device_id_t device, micropixel_sensor_info_t& info_out) const {
    info_out = {};
    info_out.size = sizeof(info_out);
    info_out.device = device;
    info_out.placement = MICROPIXEL_SENSOR_PLACEMENT_BUILT_IN;
    info_out.value_count = 3U;
    if (device == peripheral_ids::kAcceleration && acceleration_ != nullptr) {
        info_out.kind = MICROPIXEL_SENSOR_ACCELERATION;
        info_out.minimum_interval_us = 2500U;
        info_out.maximum_interval_us = 60000000U;
        return MICROPIXEL_STATUS_OK;
    }
    if (device == peripheral_ids::kMagneticField && magnetic_field_ != nullptr) {
        info_out.kind = MICROPIXEL_SENSOR_MAGNETIC_FIELD;
        info_out.minimum_interval_us = 5000U;
        info_out.maximum_interval_us = 60000000U;
        return MICROPIXEL_STATUS_OK;
    }
    return MICROPIXEL_STATUS_NOT_FOUND;
}

int32_t SensorBackend::ConfigureOnWorker(micropixel_device_id_t device, uint32_t interval_us) {
    if (device == peripheral_ids::kAcceleration && acceleration_ != nullptr) {
        uint8_t control = kSc7Sample10Hz;
        if (interval_us <= 2500U) {
            control = kSc7Sample400Hz;
        } else if (interval_us <= 5000U) {
            control = kSc7Sample200Hz;
        } else if (interval_us <= 10000U) {
            control = kSc7Sample100Hz;
        } else if (interval_us <= 20000U) {
            control = kSc7Sample50Hz;
        }
        return WriteRegister(acceleration_, kSc7Control1Register, control) ? MICROPIXEL_STATUS_OK
                                                                           : MICROPIXEL_STATUS_INTERNAL;
    }
    if (device == peripheral_ids::kMagneticField && magnetic_field_ != nullptr) {
        uint8_t control2 = kQmcControl2Normal10Hz;
        if (interval_us <= 5000U) {
            control2 = kQmcControl2Normal200Hz;
        } else if (interval_us <= 10000U) {
            control2 = kQmcControl2Normal100Hz;
        } else if (interval_us <= 20000U) {
            control2 = kQmcControl2Normal50Hz;
        }
        const bool configured = WriteRegister(magnetic_field_, kQmcControl1Register, kQmcSuspendMode) &&
                                WriteRegister(magnetic_field_, kQmcControl2Register, control2) &&
                                WriteRegister(magnetic_field_, kQmcControl1Register, kQmcLowPowerOsr | kQmcNormalMode);
        return configured ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
    }
    return MICROPIXEL_STATUS_NOT_FOUND;
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
    if (device == peripheral_ids::kAcceleration && acceleration_ != nullptr) {
        (void)WriteRegister(acceleration_, kSc7Control1Register, kSc7PowerDown);
    } else if (device == peripheral_ids::kMagneticField && magnetic_field_ != nullptr) {
        (void)WriteRegister(magnetic_field_, kQmcControl1Register, kQmcSuspendMode);
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
    if (device == peripheral_ids::kAcceleration && acceleration_ != nullptr) {
        return &acceleration_sampler_;
    }
    if (device == peripheral_ids::kMagneticField && magnetic_field_ != nullptr) {
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

int32_t SensorBackend::ReadAcceleration(device::SensorValues& values_out) const {
    uint8_t bytes[6]{};
    if (!ReadRegisters(acceleration_, kSc7OutputXLowRegister | kSc7AutoIncrement, bytes, sizeof(bytes))) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis) {
        const int16_t raw = Signed16(bytes[axis * 2U], bytes[axis * 2U + 1U]);
        values_out.values[axis] = static_cast<float>(raw >> 4) * kMetersPerSecondSquaredPerMilliG;
    }
    return MICROPIXEL_STATUS_OK;
}

int32_t SensorBackend::ReadMagneticField(device::SensorValues& values_out) const {
    uint8_t bytes[6]{};
    if (!ReadRegisters(magnetic_field_, kQmcOutputXLowRegister, bytes, sizeof(bytes))) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis) {
        values_out.values[axis] =
            static_cast<float>(Signed16(bytes[axis * 2U], bytes[axis * 2U + 1U])) * kMicroteslaPerLsb;
    }
    return MICROPIXEL_STATUS_OK;
}

}  // namespace micropixel::platform::metalio_claw4
