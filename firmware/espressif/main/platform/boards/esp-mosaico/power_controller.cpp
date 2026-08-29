#include "platform/boards/esp-mosaico/power_controller.hpp"

#include <cinttypes>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/boards/esp-mosaico/board_config.hpp"
#include "work/task_policy.hpp"

namespace micropixel::platform::esp_mosaico {
namespace {

constexpr char kTag[] = "mosaico_power";
constexpr uint32_t kCodecPowerStabilizationMs = 10U;
constexpr uint32_t kEventQueueCapacity = 8U;
constexpr uint32_t kWorkerStackBytes = 4096U;
constexpr BaseType_t kWorkerCore = 0;
constexpr uint32_t kDebounceMs = 20U;
constexpr uint64_t kLongPressUs = 2000000U;
constexpr uint64_t kClickGuardUs = 2000000U;

esp_err_t ConfigureOutput(gpio_num_t pin, int level, gpio_mode_t mode = GPIO_MODE_OUTPUT) {
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, level), kTag, "preset GPIO%d failed", static_cast<int>(pin));
    gpio_config_t config{};
    config.pin_bit_mask = BIT64(pin);
    config.mode = mode;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}

}  // namespace

PowerController::~PowerController() { Stop(); }

esp_err_t PowerController::Initialize() {
    if (initialized_) {
        return ESP_OK;
    }
    event_queue_ = xQueueCreate(kEventQueueCapacity, sizeof(Event));
    worker_stopped_ = xSemaphoreCreateBinary();
    if (event_queue_ == nullptr || worker_stopped_ == nullptr) {
        Stop();
        return ESP_ERR_NO_MEM;
    }
    esp_timer_create_args_t timer_config{};
    timer_config.callback = LongPressTimer;
    timer_config.arg = this;
    timer_config.dispatch_method = ESP_TIMER_TASK;
    timer_config.name = "mosaico_power_hold";
    timer_config.skip_unhandled_events = true;
    esp_err_t status = esp_timer_create(&timer_config, &long_press_timer_);
    if (status != ESP_OK) {
        Stop();
        return status;
    }

    // Deep-sleep pad holds survive reset. Program safe inactive levels into the
    // output latches before releasing a retained state, matching the board BSP.
    status = ConfigureOutput(board::kPeripheralPower, 1);
    if (status == ESP_OK) {
        status = ConfigureOutput(board::kAudioCodecPower, 0);
    }
    if (status == ESP_OK) {
        status = ConfigureOutput(board::kPowerSwitch, 1, GPIO_MODE_INPUT_OUTPUT_OD);
    }
    if (status == ESP_OK) {
        status = gpio_hold_dis(board::kPeripheralPower);
    }
    if (status == ESP_OK) {
        status = gpio_hold_dis(board::kAudioCodecPower);
    }
    if (status == ESP_OK) {
        status = gpio_hold_dis(board::kPowerSwitch);
    }
    if (status != ESP_OK) {
        Stop();
        return status;
    }

    const esp_err_t isr_service = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (isr_service != ESP_OK && isr_service != ESP_ERR_INVALID_STATE) {
        Stop();
        return isr_service;
    }
    pressed_ = gpio_get_level(board::kPowerSwitch) == 0;
    stopping_.store(false, std::memory_order_release);
    if (xTaskCreatePinnedToCore(WorkerEntry, "mosaico_power", kWorkerStackBytes, this, task_policy::kHostPriority,
                                &worker_, kWorkerCore) != pdPASS) {
        worker_ = nullptr;
        Stop();
        return ESP_ERR_NO_MEM;
    }
    status = gpio_isr_handler_add(board::kPowerSwitch, OnEdge, this);
    if (status == ESP_OK) {
        isr_registered_ = true;
        status = gpio_set_intr_type(board::kPowerSwitch, GPIO_INTR_ANYEDGE);
    }
    if (status == ESP_OK) {
        status = gpio_intr_enable(board::kPowerSwitch);
    }
    if (status != ESP_OK) {
        Stop();
        return status;
    }

    initialized_ = true;
    peripheral_power_enabled_ = false;
    codec_power_enabled_ = false;
    status = SetPeripheralPower(true);
    if (status != ESP_OK) {
        Stop();
        return status;
    }
    ESP_LOGI(kTag, "POWER button monitor ready on GPIO%d (active-low, open-drain)",
             static_cast<int>(board::kPowerSwitch));
    return ESP_OK;
}

void PowerController::Stop() {
    if (isr_registered_) {
        (void)gpio_intr_disable(board::kPowerSwitch);
        (void)gpio_isr_handler_remove(board::kPowerSwitch);
        isr_registered_ = false;
    }
    if (long_press_timer_ != nullptr) {
        (void)esp_timer_stop_blocking(long_press_timer_, portMAX_DELAY);
    }
    if (worker_ != nullptr) {
        stopping_.store(true, std::memory_order_release);
        const Event stop{.kind = EventKind::kStop};
        (void)xQueueSend(event_queue_, &stop, 0U);
        (void)xSemaphoreTake(worker_stopped_, portMAX_DELAY);
        worker_ = nullptr;
    }
    if (long_press_timer_ != nullptr) {
        (void)esp_timer_delete(long_press_timer_);
        long_press_timer_ = nullptr;
    }
    if (event_queue_ != nullptr) {
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
    }
    if (worker_stopped_ != nullptr) {
        vSemaphoreDelete(worker_stopped_);
        worker_stopped_ = nullptr;
    }
    initialized_ = false;
}

esp_err_t PowerController::SetPeripheralPower(bool enabled) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!enabled || peripheral_power_enabled_) {
        const int level = enabled ? 0 : 1;
        ESP_RETURN_ON_ERROR(gpio_set_level(board::kPeripheralPower, level), kTag, "set peripheral power failed");
        peripheral_power_enabled_ = enabled;
        return ESP_OK;
    }

    constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
    constexpr ledc_timer_t kTimer = LEDC_TIMER_1;
    constexpr ledc_channel_t kChannel = LEDC_CHANNEL_1;
    constexpr ledc_timer_bit_t kResolution = LEDC_TIMER_8_BIT;
    constexpr uint32_t kMaximumDuty = (1U << kResolution) - 1U;

    ledc_timer_config_t timer{};
    timer.speed_mode = kSpeedMode;
    timer.duty_resolution = kResolution;
    timer.timer_num = kTimer;
    timer.freq_hz = 100000U;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), kTag, "configure peripheral power ramp timer failed");

    ledc_channel_config_t channel{};
    channel.gpio_num = board::kPeripheralPower;
    channel.speed_mode = kSpeedMode;
    channel.channel = kChannel;
    channel.timer_sel = kTimer;
    channel.duty = kMaximumDuty;
    channel.hpoint = 0U;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), kTag, "configure peripheral power ramp channel failed");

    const esp_err_t installed = ledc_fade_func_install(0U);
    esp_err_t status = installed == ESP_ERR_INVALID_STATE ? ESP_OK : installed;
    if (status == ESP_OK) {
        status = ledc_set_fade_with_time(kSpeedMode, kChannel, 0U, board::kPowerRampMs);
    }
    if (status == ESP_OK) {
        status = ledc_fade_start(kSpeedMode, kChannel, LEDC_FADE_WAIT_DONE);
    }
    if (installed == ESP_OK) {
        ledc_fade_func_uninstall();
    }
    ledc_stop(kSpeedMode, kChannel, status == ESP_OK ? 0 : 1);
    ESP_RETURN_ON_ERROR(status, kTag, "ramp peripheral power failed");
    ESP_RETURN_ON_ERROR(ConfigureOutput(board::kPeripheralPower, 0), kTag, "hold peripheral power on failed");
    peripheral_power_enabled_ = true;
    ESP_LOGI(kTag, "3V3 peripheral rail ramped on over %" PRIu32 " ms", board::kPowerRampMs);
    return ESP_OK;
}

esp_err_t PowerController::SetCodecPower(bool enabled) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(gpio_set_level(board::kAudioCodecPower, enabled ? 1 : 0), kTag, "set codec power failed");
    codec_power_enabled_ = enabled;
    ESP_LOGI(kTag, "codec 3V3 rail %s", enabled ? "on" : "off");
    return ESP_OK;
}

esp_err_t PowerController::PowerOnAudio() {
    ESP_RETURN_ON_ERROR(SetPeripheralPower(true), kTag, "ensure shared 3V3 rail is on failed");
    if (codec_power_enabled_) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(SetCodecPower(true), kTag, "enable codec 3V3 rail failed");
    vTaskDelay(pdMS_TO_TICKS(kCodecPowerStabilizationMs));
    return ESP_OK;
}

esp_err_t PowerController::PowerOffAudio() { return codec_power_enabled_ ? SetCodecPower(false) : ESP_OK; }

void PowerController::SetPowerButtonSink(device::PowerButtonSink sink, void* context) {
    portENTER_CRITICAL(&sink_lock_);
    power_button_sink_ = sink;
    power_button_context_ = context;
    portEXIT_CRITICAL(&sink_lock_);
}

void PowerController::SetPowerOffButtonSink(device::PowerOffButtonSink sink, void* context) {
    portENTER_CRITICAL(&sink_lock_);
    power_off_sink_ = sink;
    power_off_context_ = context;
    portEXIT_CRITICAL(&sink_lock_);
}

esp_err_t PowerController::PrepareForLightSleep() {
    if (IsPressed()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(gpio_wakeup_enable(board::kPowerSwitch, GPIO_INTR_LOW_LEVEL), kTag,
                        "enable power-button wake failed");
    const esp_err_t status = esp_sleep_enable_gpio_wakeup();
    if (status != ESP_OK) {
        (void)gpio_wakeup_disable(board::kPowerSwitch);
    }
    return status;
}

void PowerController::FinishLightSleep() { (void)gpio_wakeup_disable(board::kPowerSwitch); }

bool PowerController::IsPressed() const { return gpio_get_level(board::kPowerSwitch) == 0; }

bool PowerController::PowerPressOccurredAfterRequest() const {
    return press_generation_.load(std::memory_order_acquire) != request_generation_.load(std::memory_order_acquire);
}

void PowerController::GuardWakeButtonUntilRelease(uint64_t click_deadline_us) {
    suppress_click_until_us_.store(click_deadline_us, std::memory_order_release);
    if (IsPressed()) {
        wake_release_required_.store(true, std::memory_order_release);
    } else {
        wake_release_required_.store(false, std::memory_order_release);
        request_generation_.store(press_generation_.load(std::memory_order_acquire), std::memory_order_release);
    }
}

void IRAM_ATTR PowerController::OnEdge(void* context) {
    auto* power = static_cast<PowerController*>(context);
    if (power == nullptr || power->event_queue_ == nullptr) {
        return;
    }
    const Event event{.kind = EventKind::kEdge, .timestamp_us = static_cast<uint64_t>(esp_timer_get_time())};
    BaseType_t higher_priority_woken = pdFALSE;
    (void)xQueueSendFromISR(power->event_queue_, &event, &higher_priority_woken);
    if (higher_priority_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void PowerController::LongPressTimer(void* context) {
    auto* power = static_cast<PowerController*>(context);
    if (power == nullptr || power->event_queue_ == nullptr) {
        return;
    }
    const Event event{.kind = EventKind::kLongPress, .timestamp_us = static_cast<uint64_t>(esp_timer_get_time())};
    (void)xQueueSend(power->event_queue_, &event, 0U);
}

void PowerController::WorkerEntry(void* context) {
    static_cast<PowerController*>(context)->Worker();
    vTaskDelete(nullptr);
}

void PowerController::Worker() {
    for (;;) {
        Event event{};
        if (xQueueReceive(event_queue_, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (event.kind == EventKind::kStop || stopping_.load(std::memory_order_acquire)) {
            break;
        }
        if (event.kind == EventKind::kEdge) {
            vTaskDelay(pdMS_TO_TICKS(kDebounceMs));
            HandleStableLevel(gpio_get_level(board::kPowerSwitch) == 0, event.timestamp_us);
            continue;
        }
        if (event.kind == EventKind::kLongPress && pressed_ && !long_press_sent_ &&
            !wake_release_required_.load(std::memory_order_acquire)) {
            long_press_sent_ = true;
            device::PowerOffButtonSink sink = nullptr;
            void* sink_context = nullptr;
            portENTER_CRITICAL(&sink_lock_);
            sink = power_off_sink_;
            sink_context = power_off_context_;
            portEXIT_CRITICAL(&sink_lock_);
            if (sink == nullptr || !sink(sink_context, event.timestamp_us)) {
                ESP_LOGI(kTag, "two-second power-off request rejected by Host");
            } else {
                ESP_LOGI(kTag, "two-second power-off request accepted by Host");
            }
        }
    }
    (void)xSemaphoreGive(worker_stopped_);
}

void PowerController::HandleStableLevel(bool pressed, uint64_t timestamp_us) {
    if (pressed == pressed_) {
        return;
    }
    pressed_ = pressed;
    if (pressed) {
        press_started_us_ = timestamp_us;
        long_press_sent_ = false;
        press_generation_.fetch_add(1U, std::memory_order_acq_rel);
        (void)esp_timer_stop(long_press_timer_);
        (void)esp_timer_start_once(long_press_timer_, kLongPressUs);
        ESP_LOGI(kTag, "POWER button down");
        return;
    }

    (void)esp_timer_stop(long_press_timer_);
    ESP_LOGI(kTag, "POWER button up after %" PRIu64 " ms", (timestamp_us - press_started_us_) / 1000U);
    if (wake_release_required_.exchange(false, std::memory_order_acq_rel)) {
        request_generation_.store(press_generation_.load(std::memory_order_acquire), std::memory_order_release);
        return;
    }
    if (long_press_sent_ || timestamp_us <= suppress_click_until_us_.load(std::memory_order_acquire)) {
        return;
    }
    device::PowerButtonSink sink = nullptr;
    void* sink_context = nullptr;
    portENTER_CRITICAL(&sink_lock_);
    sink = power_button_sink_;
    sink_context = power_button_context_;
    portEXIT_CRITICAL(&sink_lock_);
    if (sink != nullptr && sink(sink_context, timestamp_us)) {
        request_generation_.store(press_generation_.load(std::memory_order_acquire), std::memory_order_release);
    } else {
        ESP_LOGI(kTag, "power-button click rejected by Host");
    }
    suppress_click_until_us_.store(timestamp_us + kClickGuardUs, std::memory_order_release);
}

[[noreturn]] void PowerController::PowerOff() {
    (void)gpio_intr_disable(board::kPowerSwitch);
    ESP_LOGI(kTag, "pulling POWER_SWITCH low until SAM8108 cuts power");
    for (;;) {
        (void)ConfigureOutput(board::kPowerSwitch, 0, GPIO_MODE_OUTPUT_OD);
        vTaskDelay(pdMS_TO_TICKS(100U));
    }
}

}  // namespace micropixel::platform::esp_mosaico
