#ifndef MICROPIXEL_TEST_STUB_FREERTOS_TASK_H
#define MICROPIXEL_TEST_STUB_FREERTOS_TASK_H

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "freertos/FreeRTOS.h"

inline void vTaskDelay(TickType_t ticks) { std::this_thread::sleep_for(std::chrono::milliseconds(ticks)); }

inline TickType_t xTaskGetTickCount() {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<TickType_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

struct MicropixelTestTask final {
    std::thread thread;
    std::mutex notification_mutex;
    std::condition_variable notification_ready;
    uint32_t notifications{};
};

using TaskHandle_t = MicropixelTestTask*;
using TaskFunction_t = void (*)(void*);
inline thread_local TaskHandle_t micropixel_test_current_task{};

inline TaskHandle_t xTaskGetCurrentTaskHandle() { return micropixel_test_current_task; }

inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char*, uint32_t, void* context, UBaseType_t,
                                          TaskHandle_t* task_out, BaseType_t) {
    if (function == nullptr || task_out == nullptr) {
        return pdFALSE;
    }
    auto* task = new MicropixelTestTask;
    task->thread = std::thread([function, context, task] {
        micropixel_test_current_task = task;
        function(context);
        micropixel_test_current_task = nullptr;
    });
    task->thread.detach();
    *task_out = task;
    return pdTRUE;
}

inline void vTaskDelete(TaskHandle_t) {}

inline void xTaskNotifyGive(TaskHandle_t task) {
    if (task == nullptr) {
        return;
    }
    {
        std::lock_guard lock(task->notification_mutex);
        ++task->notifications;
    }
    task->notification_ready.notify_one();
}

inline void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t* higher_priority_task_woken) {
    xTaskNotifyGive(task);
    if (higher_priority_task_woken != nullptr) {
        *higher_priority_task_woken = pdTRUE;
    }
}

inline uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t timeout) {
    TaskHandle_t task = micropixel_test_current_task;
    if (task == nullptr) {
        return 0U;
    }
    std::unique_lock lock(task->notification_mutex);
    const auto notified = [task] { return task->notifications != 0U; };
    if (timeout == portMAX_DELAY) {
        task->notification_ready.wait(lock, notified);
    } else if (timeout == 0U) {
        if (!notified()) {
            return 0U;
        }
    } else if (!task->notification_ready.wait_for(lock, std::chrono::milliseconds(timeout), notified)) {
        return 0U;
    }
    const uint32_t value = task->notifications;
    if (clear_on_exit == pdTRUE) {
        task->notifications = 0U;
    } else {
        --task->notifications;
    }
    return value;
}

#endif
