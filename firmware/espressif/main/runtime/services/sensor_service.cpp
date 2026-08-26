#include "runtime/services/sensor_service.hpp"

#include <cstring>

#include "runtime/services/timer_service.hpp"

namespace micropixel::runtime {

SensorService::SensorService(device::SensorsService& sensors, TimerService& clock)
    : sensors_(sensors), clock_(clock), mutex_(xSemaphoreCreateMutex()) {}

SensorService::~SensorService() {
    Shutdown();
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
    }
}

bool SensorService::TakeLock() { return mutex_ != nullptr && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE; }

void SensorService::GiveLock() { (void)xSemaphoreGive(mutex_); }

SensorService::Slot* SensorService::Find(micropixel_sensor_handle_t handle) {
    const uint32_t encoded_index = handle & 0xffU;
    if (encoded_index == 0U || encoded_index > limits::kMaxSensorHandles) {
        return nullptr;
    }
    Slot& slot = slots_[encoded_index - 1U];
    return slot.handle == handle ? &slot : nullptr;
}

ServiceResult<micropixel_sensor_open_response_t> SensorService::Open(micropixel_device_id_t device,
                                                                     uint16_t expected_kind) {
    auto info = sensors_.GetInfo(device);
    if (!info) {
        return FailService<micropixel_sensor_open_response_t>(info.error().status);
    }
    if (expected_kind == 0U || info->kind != expected_kind) {
        return FailService<micropixel_sensor_open_response_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (!TakeLock()) {
        return FailService<micropixel_sensor_open_response_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    if (suspended_ || shut_down_) {
        GiveLock();
        return FailService<micropixel_sensor_open_response_t>(MICROPIXEL_STATUS_CLOSED);
    }
    for (const Slot& existing : slots_) {
        if (existing.handle != 0U && existing.device == device) {
            GiveLock();
            return FailService<micropixel_sensor_open_response_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        }
    }
    uint32_t selected_index = limits::kMaxSensorHandles;
    for (uint32_t index = 0U; index < limits::kMaxSensorHandles; ++index) {
        if (slots_[index].handle == 0U) {
            selected_index = index;
            break;
        }
    }
    if (selected_index == limits::kMaxSensorHandles) {
        GiveLock();
        return FailService<micropixel_sensor_open_response_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    uint32_t interval_us = kDefaultSampleIntervalUs;
    if (interval_us < info->minimum_interval_us) {
        interval_us = info->minimum_interval_us;
    }
    if (interval_us > info->maximum_interval_us) {
        interval_us = info->maximum_interval_us;
    }
    auto started = sensors_.Start(device, interval_us);
    if (!started) {
        GiveLock();
        return FailService<micropixel_sensor_open_response_t>(started.error().status);
    }

    Slot& slot = slots_[selected_index];
    ++slot.generation;
    if (slot.generation == 0U) {
        ++slot.generation;
    }
    slot.device = device;
    slot.kind = info->kind;
    slot.interval_us = interval_us;
    slot.minimum_interval_us = info->minimum_interval_us;
    slot.maximum_interval_us = info->maximum_interval_us;
    slot.handle = (slot.generation << 8U) | (selected_index + 1U);
    const micropixel_sensor_handle_t handle = slot.handle;
    GiveLock();

    micropixel_sensor_open_response_t response{};
    response.size = sizeof(response);
    response.kind = info->kind;
    response.sensor = handle;
    response.device = device;
    return response;
}

ServiceResult<micropixel_sensor_reading_t> SensorService::Read(micropixel_sensor_handle_t sensor) {
    if (!TakeLock()) {
        return FailService<micropixel_sensor_reading_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    Slot* slot = Find(sensor);
    if (slot == nullptr) {
        GiveLock();
        return FailService<micropixel_sensor_reading_t>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    const micropixel_device_id_t device = slot->device;
    const uint16_t kind = slot->kind;
    GiveLock();
    auto values = sensors_.Read(device);
    if (!values) {
        return FailService<micropixel_sensor_reading_t>(values.error().status);
    }
    micropixel_sensor_reading_t reading{};
    reading.size = sizeof(reading);
    reading.kind = kind;
    reading.sensor = sensor;
    reading.device = device;
    reading.timestamp_us = clock_.FromGlobalTime(values->timestamp_us);
    std::memcpy(reading.values, values->values, sizeof(reading.values));
    return reading;
}

ServiceResult<void> SensorService::SetSampleInterval(micropixel_sensor_handle_t sensor, uint64_t interval_us) {
    if (!TakeLock()) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    Slot* slot = Find(sensor);
    if (slot == nullptr) {
        GiveLock();
        return FailService<void>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    if (interval_us < slot->minimum_interval_us || interval_us > slot->maximum_interval_us ||
        interval_us > UINT32_MAX) {
        GiveLock();
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    auto configured = sensors_.Start(slot->device, static_cast<uint32_t>(interval_us));
    if (!configured) {
        GiveLock();
        return FailService<void>(configured.error().status);
    }
    slot->interval_us = static_cast<uint32_t>(interval_us);
    GiveLock();
    return {};
}

ServiceResult<void> SensorService::Release(micropixel_sensor_handle_t sensor) {
    if (!TakeLock()) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    Slot* slot = Find(sensor);
    if (slot == nullptr) {
        GiveLock();
        return FailService<void>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    const micropixel_device_id_t device = slot->device;
    const uint32_t generation = slot->generation;
    *slot = {};
    slot->generation = generation;
    GiveLock();
    sensors_.Stop(device);
    return {};
}

void SensorService::Suspend() {
    if (suspended_ || shut_down_) {
        return;
    }
    if (!TakeLock()) {
        return;
    }
    suspended_ = true;
    for (Slot& slot : slots_) {
        if (slot.handle != 0U) {
            sensors_.Stop(slot.device);
        }
    }
    GiveLock();
}

bool SensorService::Resume() {
    if (!suspended_ || shut_down_ || !TakeLock()) {
        return !shut_down_;
    }
    bool resumed = true;
    for (Slot& slot : slots_) {
        if (slot.handle == 0U) {
            continue;
        }
        auto started = sensors_.Start(slot.device, slot.interval_us);
        if (!started) {
            resumed = false;
            break;
        }
    }
    if (!resumed) {
        for (const Slot& slot : slots_) {
            if (slot.handle != 0U) {
                sensors_.Stop(slot.device);
            }
        }
        GiveLock();
        return false;
    }
    suspended_ = false;
    GiveLock();
    return true;
}

void SensorService::Shutdown() {
    if (shut_down_) {
        return;
    }
    if (!TakeLock()) {
        shut_down_ = true;
        return;
    }
    for (Slot& slot : slots_) {
        if (slot.handle != 0U) {
            sensors_.Stop(slot.device);
            const uint32_t generation = slot.generation;
            slot = {};
            slot.generation = generation;
        }
    }
    suspended_ = true;
    shut_down_ = true;
    GiveLock();
}

}  // namespace micropixel::runtime
