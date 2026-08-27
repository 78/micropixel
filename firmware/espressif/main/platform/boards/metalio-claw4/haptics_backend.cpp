#include "platform/boards/metalio-claw4/haptics_backend.hpp"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "platform/boards/metalio-claw4/peripheral_ids.hpp"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "micropixel_haptics";
constexpr gpio_num_t kMotorPin = GPIO_NUM_22;
constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_1;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_1;
constexpr ledc_timer_bit_t kResolution = LEDC_TIMER_10_BIT;
constexpr uint32_t kFrequencyHz = 5000U;
constexpr uint32_t kMaximumDuty = (1U << 10U) - 1U;
constexpr uint32_t kMaximumDurationMs = 5000U;
constexpr uint32_t kSafeMaximumDutyPerMille = 600U;

}  // namespace

HapticsBackend::~HapticsBackend() {
    (void)Stop(peripheral_ids::kHaptics);
    if (stop_timer_ != nullptr) {
        (void)esp_timer_delete(stop_timer_);
    }
    if (callback_mutex_ != nullptr) {
        vSemaphoreDelete(callback_mutex_);
    }
}

esp_err_t HapticsBackend::Initialize() {
    callback_mutex_ = xSemaphoreCreateMutex();
    if (callback_mutex_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    ledc_timer_config_t timer{};
    timer.speed_mode = kSpeedMode;
    timer.duty_resolution = kResolution;
    timer.timer_num = kTimer;
    timer.freq_hz = kFrequencyHz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    esp_err_t status = ledc_timer_config(&timer);
    if (status != ESP_OK) {
        return status;
    }

    ledc_channel_config_t channel{};
    channel.gpio_num = kMotorPin;
    channel.speed_mode = kSpeedMode;
    channel.channel = kChannel;
    channel.timer_sel = kTimer;
    channel.duty = 0U;
    status = ledc_channel_config(&channel);
    if (status != ESP_OK) {
        return status;
    }

    esp_timer_create_args_t arguments{};
    arguments.callback = OnFinished;
    arguments.arg = this;
    arguments.dispatch_method = ESP_TIMER_TASK;
    arguments.name = "micropixel_haptic";
    arguments.skip_unhandled_events = true;
    status = esp_timer_create(&arguments, &stop_timer_);
    if (status == ESP_OK) {
        ESP_LOGI(kTag, "vibration motor ready on GPIO%d", static_cast<int>(kMotorPin));
    }
    return status;
}

int32_t HapticsBackend::GetInfo(micropixel_device_id_t device, micropixel_haptics_info_t& info_out) const {
    if (device != peripheral_ids::kHaptics || stop_timer_ == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    info_out = {};
    info_out.size = sizeof(info_out);
    info_out.capabilities = MICROPIXEL_HAPTICS_CAP_VARIABLE_STRENGTH;
    info_out.device = device;
    info_out.maximum_duration_ms = kMaximumDurationMs;
    return MICROPIXEL_STATUS_OK;
}

int32_t HapticsBackend::Play(micropixel_device_id_t device, uint16_t strength_per_mille, uint32_t duration_ms) {
    if (device != peripheral_ids::kHaptics || strength_per_mille == 0U || strength_per_mille > 1000U ||
        duration_ms == 0U || duration_ms > kMaximumDurationMs || stop_timer_ == nullptr) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const esp_err_t stop_status = esp_timer_stop(stop_timer_);
    if (stop_status != ESP_OK && stop_status != ESP_ERR_INVALID_STATE) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (ApplyStrength(strength_per_mille) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    playing_.store(true, std::memory_order_release);
    if (esp_timer_start_once(stop_timer_, static_cast<uint64_t>(duration_ms) * 1000U) != ESP_OK) {
        playing_.store(false, std::memory_order_release);
        (void)ApplyStrength(0U);
        return MICROPIXEL_STATUS_INTERNAL;
    }
    return MICROPIXEL_STATUS_OK;
}

int32_t HapticsBackend::Stop(micropixel_device_id_t device) {
    if (device != peripheral_ids::kHaptics) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    playing_.store(false, std::memory_order_release);
    if (stop_timer_ != nullptr) {
        const esp_err_t timer_status = esp_timer_stop(stop_timer_);
        if (timer_status != ESP_OK && timer_status != ESP_ERR_INVALID_STATE) {
            return MICROPIXEL_STATUS_INTERNAL;
        }
    }
    if (ApplyStrength(0U) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    return MICROPIXEL_STATUS_OK;
}

void HapticsBackend::SetCompletionSink(device::HapticCompletionSink sink, void* context) {
    if (callback_mutex_ == nullptr || xSemaphoreTake(callback_mutex_, portMAX_DELAY) != pdTRUE) {
        return;
    }
    completion_context_.store(sink == nullptr ? nullptr : context, std::memory_order_release);
    completion_sink_.store(sink, std::memory_order_release);
    (void)xSemaphoreGive(callback_mutex_);
}

void HapticsBackend::OnFinished(void* context) {
    auto& backend = *static_cast<HapticsBackend*>(context);
    if (!backend.playing_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    (void)backend.ApplyStrength(0U);
    if (backend.callback_mutex_ != nullptr && xSemaphoreTake(backend.callback_mutex_, portMAX_DELAY) == pdTRUE) {
        device::HapticCompletionSink sink = backend.completion_sink_.load(std::memory_order_acquire);
        if (sink != nullptr) {
            sink(backend.completion_context_.load(std::memory_order_acquire), peripheral_ids::kHaptics,
                 static_cast<uint64_t>(esp_timer_get_time()));
        }
        (void)xSemaphoreGive(backend.callback_mutex_);
    }
}

esp_err_t HapticsBackend::ApplyStrength(uint16_t strength_per_mille) {
    const uint32_t duty = kMaximumDuty * strength_per_mille * kSafeMaximumDutyPerMille / 1000000U;
    const esp_err_t status = ledc_set_duty(kSpeedMode, kChannel, duty);
    return status == ESP_OK ? ledc_update_duty(kSpeedMode, kChannel) : status;
}

}  // namespace micropixel::platform::metalio_claw4
