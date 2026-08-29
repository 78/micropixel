#include "platform/haptics/timed_haptics_peripheral.hpp"

#include "esp_log.h"

namespace micropixel::platform::haptics {
namespace {

constexpr char kTag[] = "timed_haptics";

}  // namespace

TimedHapticsPeripheral::TimedHapticsPeripheral(HapticActuator& actuator, uint32_t capabilities,
                                               uint32_t maximum_duration_ms)
    : actuator_(actuator), capabilities_(capabilities), maximum_duration_ms_(maximum_duration_ms) {}

TimedHapticsPeripheral::~TimedHapticsPeripheral() {
    (void)Stop(kChannel);
    if (stop_timer_ != nullptr) {
        (void)esp_timer_delete(stop_timer_);
    }
    if (callback_mutex_ != nullptr) {
        vSemaphoreDelete(callback_mutex_);
    }
}

esp_err_t TimedHapticsPeripheral::Initialize() {
    if (stop_timer_ != nullptr || callback_mutex_ != nullptr || maximum_duration_ms_ == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    callback_mutex_ = xSemaphoreCreateMutex();
    if (callback_mutex_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t status = actuator_.Initialize();
    if (status != ESP_OK) {
        vSemaphoreDelete(callback_mutex_);
        callback_mutex_ = nullptr;
        return status;
    }
    status = SetStrength(0U);
    if (status != ESP_OK) {
        vSemaphoreDelete(callback_mutex_);
        callback_mutex_ = nullptr;
        return status;
    }

    esp_timer_create_args_t arguments{};
    arguments.callback = OnFinished;
    arguments.arg = this;
    arguments.dispatch_method = ESP_TIMER_TASK;
    arguments.name = "micropixel_haptic";
    arguments.skip_unhandled_events = true;
    status = esp_timer_create(&arguments, &stop_timer_);
    if (status != ESP_OK) {
        vSemaphoreDelete(callback_mutex_);
        callback_mutex_ = nullptr;
        return status;
    }
    ESP_LOGI(kTag, "haptic actuator ready");
    return ESP_OK;
}

int32_t TimedHapticsPeripheral::GetInfo(device::PeripheralChannelId channel,
                                        micropixel_haptics_info_t& info_out) const {
    if (channel != kChannel || stop_timer_ == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    info_out = {};
    info_out.size = sizeof(info_out);
    info_out.capabilities = capabilities_;
    info_out.maximum_duration_ms = maximum_duration_ms_;
    return MICROPIXEL_STATUS_OK;
}

int32_t TimedHapticsPeripheral::Play(device::PeripheralChannelId channel, uint16_t strength_per_mille,
                                     uint32_t duration_ms) {
    if (channel != kChannel) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (strength_per_mille == 0U || strength_per_mille > 1000U || duration_ms == 0U ||
        duration_ms > maximum_duration_ms_ || stop_timer_ == nullptr) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const esp_err_t stopped = esp_timer_stop(stop_timer_);
    if (stopped != ESP_OK && stopped != ESP_ERR_INVALID_STATE) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (SetStrength(strength_per_mille) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    playing_.store(true, std::memory_order_release);
    if (esp_timer_start_once(stop_timer_, static_cast<uint64_t>(duration_ms) * 1000U) != ESP_OK) {
        playing_.store(false, std::memory_order_release);
        (void)SetStrength(0U);
        return MICROPIXEL_STATUS_INTERNAL;
    }
    return MICROPIXEL_STATUS_OK;
}

int32_t TimedHapticsPeripheral::Stop(device::PeripheralChannelId channel) {
    if (channel != kChannel) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    playing_.store(false, std::memory_order_release);
    if (stop_timer_ != nullptr) {
        const esp_err_t stopped = esp_timer_stop(stop_timer_);
        if (stopped != ESP_OK && stopped != ESP_ERR_INVALID_STATE) {
            return MICROPIXEL_STATUS_INTERNAL;
        }
    }
    return SetStrength(0U) == ESP_OK ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
}

void TimedHapticsPeripheral::SetCompletionSink(device::HapticsPeripheralCompletionSink sink, void* context) {
    if (callback_mutex_ == nullptr || xSemaphoreTake(callback_mutex_, portMAX_DELAY) != pdTRUE) {
        return;
    }
    completion_context_.store(sink == nullptr ? nullptr : context, std::memory_order_release);
    completion_sink_.store(sink, std::memory_order_release);
    (void)xSemaphoreGive(callback_mutex_);
}

void TimedHapticsPeripheral::OnFinished(void* context) {
    auto& peripheral = *static_cast<TimedHapticsPeripheral*>(context);
    if (!peripheral.playing_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    (void)peripheral.SetStrength(0U);
    if (peripheral.callback_mutex_ != nullptr && xSemaphoreTake(peripheral.callback_mutex_, portMAX_DELAY) == pdTRUE) {
        device::HapticsPeripheralCompletionSink sink = peripheral.completion_sink_.load(std::memory_order_acquire);
        if (sink != nullptr) {
            sink(peripheral.completion_context_.load(std::memory_order_acquire), peripheral, kChannel,
                 static_cast<uint64_t>(esp_timer_get_time()));
        }
        (void)xSemaphoreGive(peripheral.callback_mutex_);
    }
}

esp_err_t TimedHapticsPeripheral::SetStrength(uint16_t strength_per_mille) const {
    return actuator_.SetStrength(strength_per_mille);
}

}  // namespace micropixel::platform::haptics
