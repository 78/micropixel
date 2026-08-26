#ifndef MICROPIXEL_TEST_STUB_FREERTOS_SEMPHR_H
#define MICROPIXEL_TEST_STUB_FREERTOS_SEMPHR_H

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "freertos/FreeRTOS.h"

struct StaticSemaphore_t final {
    std::mutex mutex;
    std::condition_variable ready;
    bool available{};
};

using SemaphoreHandle_t = StaticSemaphore_t*;

inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* storage) {
    if (storage == nullptr) {
        return nullptr;
    }
    storage->available = false;
    return storage;
}

inline SemaphoreHandle_t xSemaphoreCreateBinary() { return new StaticSemaphore_t; }

inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    auto* semaphore = new StaticSemaphore_t;
    semaphore->available = true;
    return semaphore;
}

inline void vSemaphoreDelete(SemaphoreHandle_t semaphore) { delete semaphore; }

inline BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    if (semaphore == nullptr) {
        return pdFALSE;
    }
    {
        std::lock_guard lock(semaphore->mutex);
        semaphore->available = true;
    }
    semaphore->ready.notify_one();
    return pdTRUE;
}

inline BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout) {
    if (semaphore == nullptr) {
        return pdFALSE;
    }
    std::unique_lock lock(semaphore->mutex);
    const auto available = [semaphore] { return semaphore->available; };
    if (timeout == portMAX_DELAY) {
        semaphore->ready.wait(lock, available);
    } else if (timeout == 0U) {
        if (!available()) {
            return pdFALSE;
        }
    } else if (!semaphore->ready.wait_for(lock, std::chrono::milliseconds(timeout), available)) {
        return pdFALSE;
    }
    semaphore->available = false;
    return pdTRUE;
}

#endif
