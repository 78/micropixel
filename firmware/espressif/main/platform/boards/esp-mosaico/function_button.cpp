#include "platform/boards/esp-mosaico/function_button.hpp"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "platform/boards/esp-mosaico/board_config.hpp"
#include "work/task_policy.hpp"

namespace micropixel::platform::esp_mosaico {
namespace {

constexpr char kTag[] = "mosaico_button";
constexpr uint32_t kEdgeQueueCapacity = 8U;
constexpr uint32_t kWorkerStackBytes = 3072U;
constexpr uint32_t kDebounceMs = 20U;
constexpr BaseType_t kWorkerCore = 0;

}  // namespace

FunctionButton::~FunctionButton() { Stop(); }

esp_err_t FunctionButton::Initialize(device::Input& input) {
    if (input_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    edge_queue_ = xQueueCreate(kEdgeQueueCapacity, sizeof(EdgeRecord));
    worker_stopped_ = xSemaphoreCreateBinary();
    if (edge_queue_ == nullptr || worker_stopped_ == nullptr) {
        Stop();
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t config{};
    config.pin_bit_mask = BIT64(board::kFunctionButton);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_ANYEDGE;
    const esp_err_t configured = gpio_config(&config);
    if (configured != ESP_OK) {
        Stop();
        return configured;
    }

    const esp_err_t isr_status = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (isr_status != ESP_OK && isr_status != ESP_ERR_INVALID_STATE) {
        Stop();
        return isr_status;
    }
    input_ = &input;
    pressed_ = gpio_get_level(board::kFunctionButton) == 0;
    stopping_.store(false, std::memory_order_release);
    if (xTaskCreatePinnedToCore(WorkerEntry, "mosaico_button", kWorkerStackBytes, this, task_policy::kHostPriority,
                                &worker_, kWorkerCore) != pdPASS) {
        worker_ = nullptr;
        Stop();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t status = gpio_isr_handler_add(board::kFunctionButton, OnEdge, this);
    if (status == ESP_OK) {
        isr_registered_ = true;
        status = gpio_intr_enable(board::kFunctionButton);
    }
    if (status != ESP_OK) {
        Stop();
        return status;
    }
    ESP_LOGI(kTag, "function button ready on GPIO%d", static_cast<int>(board::kFunctionButton));
    return ESP_OK;
}

void FunctionButton::Stop() {
    if (isr_registered_) {
        (void)gpio_intr_disable(board::kFunctionButton);
        (void)gpio_isr_handler_remove(board::kFunctionButton);
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

void IRAM_ATTR FunctionButton::OnEdge(void* context) {
    auto* button = static_cast<FunctionButton*>(context);
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

void FunctionButton::WorkerEntry(void* context) {
    static_cast<FunctionButton*>(context)->Worker();
    vTaskDelete(nullptr);
}

void FunctionButton::Worker() {
    while (!stopping_.load(std::memory_order_acquire)) {
        EdgeRecord record{};
        if (xQueueReceive(edge_queue_, &record, portMAX_DELAY) != pdTRUE || record.timestamp_us == UINT64_MAX) {
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(kDebounceMs));
        const bool stable_pressed = gpio_get_level(board::kFunctionButton) == 0;
        if (stable_pressed == pressed_) {
            continue;
        }
        pressed_ = stable_pressed;
        if (input_ != nullptr) {
            (void)input_->InjectKey({
                .timestamp_us = static_cast<uint64_t>(esp_timer_get_time()),
                .code = device::KeyCode::kConfirm,
                .phase = stable_pressed ? device::KeyPhase::kDown : device::KeyPhase::kUp,
                .repeat_count = 0U,
            });
        }
    }
    if (worker_stopped_ != nullptr) {
        (void)xSemaphoreGive(worker_stopped_);
    }
}

}  // namespace micropixel::platform::esp_mosaico
