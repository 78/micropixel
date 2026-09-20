// SPDX-License-Identifier: Apache-2.0
#include "platform/sensors/polled_vector_sensor_peripheral.hpp"

#include "esp_log.h"
#include "freertos/task.h"

namespace micropixel::platform::sensors {

PolledVectorSensorPeripheral::~PolledVectorSensorPeripheral() {
    if (executor_ == nullptr) {
        return;
    }
    for (Channel& channel : channels_) {
        Stop(channel.id_);
        if (channel.timer_ != nullptr) {
            (void)esp_timer_delete(channel.timer_);
            channel.timer_ = nullptr;
        }
        channel.owner_ = nullptr;
    }
}

esp_err_t PolledVectorSensorPeripheral::Initialize(buses::I2cExecutor& executor) {
    if (executor_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channels_.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0U; index < channels_.size(); ++index) {
        const Channel& channel = channels_[index];
        if (channel.owner_ != nullptr || channel.timer_name_ == nullptr ||
            (channel.kind_ != MICROPIXEL_SENSOR_ACCELERATION && channel.kind_ != MICROPIXEL_SENSOR_ANGULAR_VELOCITY &&
             channel.kind_ != MICROPIXEL_SENSOR_MAGNETIC_FIELD)) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t previous = 0U; previous < index; ++previous) {
            if (channels_[previous].id_ == channel.id_) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    executor_ = &executor;
    for (Channel& channel : channels_) {
        channel.owner_ = this;
        if (!channel.driver_.available()) {
            continue;
        }
        esp_timer_create_args_t arguments{};
        arguments.callback = TimerExpired;
        arguments.arg = &channel;
        arguments.dispatch_method = ESP_TIMER_TASK;
        arguments.name = channel.timer_name_;
        arguments.skip_unhandled_events = true;
        if (esp_timer_create(&arguments, &channel.timer_) != ESP_OK) {
            ESP_LOGW(log_tag_, "%s timer unavailable", channel.timer_name_);
        }
    }
    return ESP_OK;
}

PolledVectorSensorPeripheral::Channel* PolledVectorSensorPeripheral::FindChannel(
    device::PeripheralChannelId channel) const {
    for (Channel& entry : channels_) {
        if (entry.id_ == channel) {
            return &entry;
        }
    }
    return nullptr;
}

int32_t PolledVectorSensorPeripheral::GetInfo(device::PeripheralChannelId channel,
                                              micropixel_sensor_info_t& info_out) const {
    info_out = {};
    info_out.size = sizeof(info_out);
    info_out.placement = MICROPIXEL_SENSOR_PLACEMENT_BUILT_IN;
    info_out.value_count = 3U;
    const Channel* entry = FindChannel(channel);
    if (entry == nullptr || !entry->driver_.available()) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    info_out.kind = entry->kind_;
    info_out.min_interval_us = entry->driver_.min_interval_us();
    info_out.max_interval_us = 60000000U;
    return MICROPIXEL_STATUS_OK;
}

esp_err_t PolledVectorSensorPeripheral::Quiesce(Channel& channel) {
    channel.active_.store(false, std::memory_order_release);
    if (channel.timer_ != nullptr) {
        (void)esp_timer_stop_blocking(channel.timer_, portMAX_DELAY);
    }
    if (executor_ == nullptr || !channel.pending_.load(std::memory_order_acquire)) {
        return ESP_OK;
    }
    // Sampling jobs use the low-priority FIFO. A normal-priority Invoke can
    // overtake them, so only a low-priority barrier drains their references.
    esp_err_t status;
    do {
        status = executor_->Invoke(buses::I2cExecutor::Priority::kLow, [](void*) { return ESP_OK; }, nullptr);
        if (status == ESP_ERR_NO_MEM) {
            vTaskDelay(1U);
        }
    } while (status == ESP_ERR_NO_MEM);
    return status;
}

void PolledVectorSensorPeripheral::ResetCache(Channel& channel) {
    portENTER_CRITICAL(&cache_lock_);
    channel.latest_ = {};
    channel.status_ = MICROPIXEL_STATUS_WOULD_BLOCK;
    portEXIT_CRITICAL(&cache_lock_);
}

int32_t PolledVectorSensorPeripheral::Start(device::PeripheralChannelId channel, uint32_t interval_us) {
    Channel* entry = FindChannel(channel);
    if (entry == nullptr || !entry->driver_.available() || entry->timer_ == nullptr || executor_ == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (Quiesce(*entry) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    struct Request final {
        drivers::VectorSensor& driver;
        uint32_t interval_us;
    } request{entry->driver_, interval_us};
    const esp_err_t configured = executor_->Invoke(
        buses::I2cExecutor::Priority::kNormal,
        [](void* context) {
            auto& requested = *static_cast<Request*>(context);
            return requested.driver.Configure(requested.interval_us);
        },
        &request);
    if (configured != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    ResetCache(*entry);
    entry->active_.store(true, std::memory_order_release);
    if (esp_timer_start_periodic(entry->timer_, interval_us) != ESP_OK) {
        Stop(channel);
        return MICROPIXEL_STATUS_INTERNAL;
    }
    return MICROPIXEL_STATUS_OK;
}

int32_t PolledVectorSensorPeripheral::Read(device::PeripheralChannelId channel, device::SensorValues& values_out) {
    values_out = {};
    const Channel* entry = FindChannel(channel);
    if (entry == nullptr || !entry->driver_.available()) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    portENTER_CRITICAL(&cache_lock_);
    const int32_t status = entry->status_;
    values_out = entry->latest_;
    portEXIT_CRITICAL(&cache_lock_);
    return status;
}

void PolledVectorSensorPeripheral::Stop(device::PeripheralChannelId channel) {
    Channel* entry = FindChannel(channel);
    if (entry == nullptr || executor_ == nullptr) {
        return;
    }
    if (Quiesce(*entry) != ESP_OK) {
        ESP_LOGW(log_tag_, "%s sampling could not drain", entry->timer_name_);
        return;
    }
    ResetCache(*entry);
    if (entry->driver_.available()) {
        (void)executor_->Invoke(
            buses::I2cExecutor::Priority::kNormal,
            [](void* context) { return static_cast<Channel*>(context)->driver_.Suspend(); }, entry);
    }
}

void PolledVectorSensorPeripheral::TimerExpired(void* context) {
    auto& channel = *static_cast<Channel*>(context);
    if (!channel.active_.load(std::memory_order_acquire) ||
        channel.pending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!channel.owner_->executor_->Post(buses::I2cExecutor::Priority::kLow, SampleOnWorker, &channel)) {
        channel.pending_.store(false, std::memory_order_release);
    }
}

esp_err_t PolledVectorSensorPeripheral::SampleOnWorker(void* context) {
    auto& channel = *static_cast<Channel*>(context);
    esp_err_t status = ESP_OK;
    if (channel.active_.load(std::memory_order_acquire)) {
        float vector[3]{};
        status = channel.driver_.Read(vector);
        device::SensorValues values{};
        values.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
        for (uint32_t axis = 0U; axis < 3U; ++axis) {
            values.values[axis] = vector[axis];
        }
        if (channel.active_.load(std::memory_order_acquire)) {
            portENTER_CRITICAL(&channel.owner_->cache_lock_);
            if (status == ESP_OK) {
                channel.latest_ = values;
            }
            channel.status_ = status == ESP_OK ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
            portEXIT_CRITICAL(&channel.owner_->cache_lock_);
        }
    }
    // Keep pending set for the complete job, so Stop also waits for a running
    // read and a timer cannot enqueue another read while this one is executing.
    channel.pending_.store(false, std::memory_order_release);
    return status;
}

}  // namespace micropixel::platform::sensors
