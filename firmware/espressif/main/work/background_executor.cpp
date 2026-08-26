#include "work/background_executor.hpp"

#include "esp_log.h"
#include "task_policy.hpp"

namespace micropixel::work {
namespace {

constexpr char kTag[] = "micropixel_background";
constexpr char kTaskName[] = "micropixel_bg";
constexpr uint32_t kTaskStackBytes = 8U * 1024U;
constexpr BaseType_t kTaskCore = 0;

}  // namespace

BackgroundExecutor::BackgroundExecutor()
    : queue_(xQueueCreateStatic(kQueueCapacity, sizeof(Job), queue_bytes_.data(), &queue_storage_)),
      stopped_(xSemaphoreCreateBinaryStatic(&stopped_storage_)) {
    if (queue_ == nullptr || stopped_ == nullptr ||
        xTaskCreatePinnedToCore(WorkerEntry, kTaskName, kTaskStackBytes, this, task_policy::kAssetWorkerPriority,
                                &worker_, kTaskCore) != pdPASS) {
        worker_ = nullptr;
        ESP_LOGE(kTag, "unable to create shared background executor");
    }
}

BackgroundExecutor::~BackgroundExecutor() { Shutdown(); }

bool BackgroundExecutor::valid() const { return queue_ != nullptr && stopped_ != nullptr && worker_ != nullptr; }

bool BackgroundExecutor::Submit(Function function, void* context) {
    if (function == nullptr || !valid() || stopping_.load(std::memory_order_acquire)) {
        return false;
    }
    const Job job{function, context};
    return xQueueSend(queue_, &job, 0U) == pdTRUE;
}

void BackgroundExecutor::Shutdown() {
    if (stopping_.exchange(true, std::memory_order_acq_rel) || worker_ == nullptr) {
        return;
    }
    const Job stop{};
    (void)xQueueSend(queue_, &stop, portMAX_DELAY);
    (void)xSemaphoreTake(stopped_, portMAX_DELAY);
    worker_ = nullptr;
}

void BackgroundExecutor::WorkerEntry(void* context) {
    static_cast<BackgroundExecutor*>(context)->WorkerLoop();
    vTaskDelete(nullptr);
}

void BackgroundExecutor::WorkerLoop() {
    for (;;) {
        Job job{};
        if (xQueueReceive(queue_, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job.function == nullptr) {
            break;
        }
        job.function(job.context);
    }
    (void)xSemaphoreGive(stopped_);
}

}  // namespace micropixel::work
