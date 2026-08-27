#include "platform/common/i2c_executor.hpp"

#include "esp_log.h"
#include "task_policy.hpp"

namespace micropixel::platform::common {
namespace {

constexpr char kTag[] = "micropixel_i2c";

constexpr uint32_t PriorityIndex(I2cExecutor::Priority priority) { return static_cast<uint32_t>(priority); }

}  // namespace

I2cExecutor::~I2cExecutor() {
    Stop();
    if (completion_.ready != nullptr) {
        vSemaphoreDelete(completion_.ready);
    }
    if (invoke_mutex_ != nullptr) {
        vSemaphoreDelete(invoke_mutex_);
    }
    if (worker_stopped_ != nullptr) {
        vSemaphoreDelete(worker_stopped_);
    }
}

esp_err_t I2cExecutor::Initialize() {
    if (worker_.load(std::memory_order_acquire) != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    for (uint32_t index = 0U; index < queues_.size(); ++index) {
        queues_[index] =
            xQueueCreateStatic(kQueueCapacity, sizeof(Job), queue_bytes_[index].data(), &queue_storage_[index]);
        if (queues_[index] == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    invoke_mutex_ = xSemaphoreCreateMutex();
    completion_.ready = xSemaphoreCreateBinary();
    worker_stopped_ = xSemaphoreCreateBinary();
    if (invoke_mutex_ == nullptr || completion_.ready == nullptr || worker_stopped_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    stopping_.store(false, std::memory_order_release);
    TaskHandle_t worker = nullptr;
    if (xTaskCreatePinnedToCore(WorkerEntry, "micropixel_i2c", kWorkerStackBytes, this, task_policy::kI2cPriority,
                                &worker, kWorkerCore) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    worker_.store(worker, std::memory_order_release);
    ESP_LOGI(kTag, "shared I2C executor ready: queues=3x%u stack=%u core=%d priority=%u", kQueueCapacity,
             kWorkerStackBytes, kWorkerCore, static_cast<unsigned>(task_policy::kI2cPriority));
    return ESP_OK;
}

esp_err_t I2cExecutor::Invoke(Priority priority, Operation operation, void* context) {
    if (operation == nullptr || stopping_.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskGetCurrentTaskHandle() == worker_.load(std::memory_order_acquire)) {
        return operation(context);
    }
    if (invoke_mutex_ == nullptr || xSemaphoreTake(invoke_mutex_, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    while (xSemaphoreTake(completion_.ready, 0U) == pdTRUE) {
    }
    completion_.result = ESP_FAIL;
    TaskHandle_t worker = worker_.load(std::memory_order_acquire);
    if (worker == nullptr) {
        (void)xSemaphoreGive(invoke_mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    const Job job{operation, context, &completion_};
    QueueHandle_t queue = Queue(priority);
    if (queue == nullptr || xQueueSend(queue, &job, 0U) != pdTRUE) {
        (void)xSemaphoreGive(invoke_mutex_);
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(worker);
    if (xSemaphoreTake(completion_.ready, portMAX_DELAY) != pdTRUE) {
        (void)xSemaphoreGive(invoke_mutex_);
        return ESP_FAIL;
    }
    const esp_err_t result = completion_.result;
    (void)xSemaphoreGive(invoke_mutex_);
    return result;
}

bool I2cExecutor::Post(Priority priority, Operation operation, void* context) {
    if (operation == nullptr || stopping_.load(std::memory_order_acquire)) {
        return false;
    }
    TaskHandle_t worker = worker_.load(std::memory_order_acquire);
    if (worker == nullptr) {
        return false;
    }
    const Job job{operation, context, nullptr};
    QueueHandle_t queue = Queue(priority);
    if (queue == nullptr || xQueueSend(queue, &job, 0U) != pdTRUE) {
        return false;
    }
    xTaskNotifyGive(worker);
    return true;
}

bool IRAM_ATTR I2cExecutor::PostFromIsr(Priority priority, Operation operation, void* context,
                                        BaseType_t* higher_priority_task_woken) {
    if (operation == nullptr || stopping_.load(std::memory_order_relaxed)) {
        return false;
    }
    const Job job{operation, context, nullptr};
    const uint32_t queue_index = static_cast<uint32_t>(priority);
    QueueHandle_t queue = queue_index < queues_.size() ? queues_[queue_index] : nullptr;
    TaskHandle_t worker = worker_.load(std::memory_order_relaxed);
    if (queue == nullptr || worker == nullptr || xQueueSendFromISR(queue, &job, higher_priority_task_woken) != pdTRUE) {
        return false;
    }
    vTaskNotifyGiveFromISR(worker, higher_priority_task_woken);
    return true;
}

QueueHandle_t I2cExecutor::Queue(Priority priority) const {
    const uint32_t index = PriorityIndex(priority);
    return index < queues_.size() ? queues_[index] : nullptr;
}

bool I2cExecutor::Receive(Job& job_out) {
    for (QueueHandle_t queue : queues_) {
        if (xQueueReceive(queue, &job_out, 0U) == pdTRUE) {
            return true;
        }
    }
    return false;
}

void I2cExecutor::WorkerEntry(void* context) {
    if (context != nullptr) {
        static_cast<I2cExecutor*>(context)->WorkerLoop();
    }
    vTaskDelete(nullptr);
}

void I2cExecutor::WorkerLoop() {
    while (!stopping_.load(std::memory_order_acquire)) {
        Job job{};
        if (!Receive(job)) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        const esp_err_t result = job.operation == nullptr ? ESP_ERR_INVALID_ARG : job.operation(job.context);
        if (job.completion != nullptr) {
            job.completion->result = result;
            (void)xSemaphoreGive(job.completion->ready);
        }
    }
    (void)xSemaphoreGive(worker_stopped_);
}

void I2cExecutor::Stop() {
    TaskHandle_t worker = worker_.load(std::memory_order_acquire);
    if (worker == nullptr) {
        return;
    }
    stopping_.store(true, std::memory_order_release);
    xTaskNotifyGive(worker);
    (void)xSemaphoreTake(worker_stopped_, portMAX_DELAY);
    worker_.store(nullptr, std::memory_order_release);
}

}  // namespace micropixel::platform::common
