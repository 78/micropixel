#include "platform/input/gpio_key_input.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "platform/gpio/gpio_isr_service.hpp"
#include "work/task_policy.hpp"

namespace micropixel::platform::input {
namespace {

constexpr uint32_t kEdgeQueueCapacity = 8U;
constexpr uint32_t kWorkerStackBytes = 3072U;

}  // namespace

GpioKeyInput::~GpioKeyInput() { Stop(); }

esp_err_t GpioKeyInput::Initialize(device::Input& input) {
    if (input_ != nullptr || !GPIO_IS_VALID_GPIO(config_.pin) || config_.log_tag == nullptr ||
        config_.task_name == nullptr || config_.debounce_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    edge_queue_ = xQueueCreate(kEdgeQueueCapacity, sizeof(EdgeRecord));
    worker_stopped_ = xSemaphoreCreateBinary();
    if (edge_queue_ == nullptr || worker_stopped_ == nullptr) {
        Stop();
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t pin_config{};
    pin_config.pin_bit_mask = BIT64(config_.pin);
    pin_config.mode = GPIO_MODE_INPUT;
    pin_config.pull_up_en = config_.active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    pin_config.pull_down_en = config_.active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    pin_config.intr_type = GPIO_INTR_ANYEDGE;
    const esp_err_t configured = gpio_config(&pin_config);
    if (configured != ESP_OK) {
        Stop();
        return configured;
    }

    const esp_err_t isr_status = gpio::EnsureIsrServiceInstalled();
    if (isr_status != ESP_OK) {
        Stop();
        return isr_status;
    }
    input_ = &input;
    pressed_ = IsPressed();
    stopping_.store(false, std::memory_order_release);
    if (xTaskCreatePinnedToCore(WorkerEntry, config_.task_name, kWorkerStackBytes, this, task_policy::kHostPriority,
                                &worker_, config_.task_core) != pdPASS) {
        worker_ = nullptr;
        Stop();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t status = gpio_isr_handler_add(config_.pin, OnEdge, this);
    if (status == ESP_OK) {
        isr_registered_ = true;
        status = gpio_intr_enable(config_.pin);
    }
    if (status != ESP_OK) {
        Stop();
        return status;
    }
    ESP_LOGI(config_.log_tag, "key input ready: GPIO%d code=%u", static_cast<int>(config_.pin),
             static_cast<unsigned>(config_.code));
    return ESP_OK;
}

bool GpioKeyInput::IsPressed() const {
    const bool high = gpio_get_level(config_.pin) != 0;
    return config_.active_low ? !high : high;
}

void GpioKeyInput::Stop() {
    if (isr_registered_) {
        (void)gpio_intr_disable(config_.pin);
        (void)gpio_isr_handler_remove(config_.pin);
        isr_registered_ = false;
    }
    if (worker_ != nullptr) {
        stopping_.store(true, std::memory_order_release);
        const EdgeRecord stop{UINT64_MAX};
        (void)xQueueSend(edge_queue_, &stop, 0U);
        (void)xSemaphoreTake(worker_stopped_, portMAX_DELAY);
        worker_ = nullptr;
    }
    if (edge_queue_ != nullptr) {
        vQueueDelete(edge_queue_);
        edge_queue_ = nullptr;
    }
    if (worker_stopped_ != nullptr) {
        vSemaphoreDelete(worker_stopped_);
        worker_stopped_ = nullptr;
    }
    input_ = nullptr;
}

void IRAM_ATTR GpioKeyInput::OnEdge(void* context) {
    auto* button = static_cast<GpioKeyInput*>(context);
    if (button == nullptr || button->edge_queue_ == nullptr) {
        return;
    }
    const EdgeRecord record{static_cast<uint64_t>(esp_timer_get_time())};
    BaseType_t higher_priority_woken = pdFALSE;
    (void)xQueueSendFromISR(button->edge_queue_, &record, &higher_priority_woken);
    if (higher_priority_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void GpioKeyInput::WorkerEntry(void* context) {
    static_cast<GpioKeyInput*>(context)->Worker();
    vTaskDelete(nullptr);
}

void GpioKeyInput::Worker() {
    while (!stopping_.load(std::memory_order_acquire)) {
        EdgeRecord record{};
        if (xQueueReceive(edge_queue_, &record, portMAX_DELAY) != pdTRUE || record.timestamp_us == UINT64_MAX) {
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(config_.debounce_ms));
        const bool stable_pressed = IsPressed();
        if (stable_pressed == pressed_) {
            continue;
        }
        pressed_ = stable_pressed;
        if (input_ != nullptr) {
            (void)input_->InjectKey({
                .timestamp_us = static_cast<uint64_t>(esp_timer_get_time()),
                .code = config_.code,
                .phase = stable_pressed ? device::KeyPhase::kDown : device::KeyPhase::kUp,
                .repeat_count = 0U,
            });
        }
    }
    if (worker_stopped_ != nullptr) {
        (void)xSemaphoreGive(worker_stopped_);
    }
}

}  // namespace micropixel::platform::input
