#ifndef MICROPIXEL_TEST_STUB_FREERTOS_QUEUE_H
#define MICROPIXEL_TEST_STUB_FREERTOS_QUEUE_H

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "freertos/FreeRTOS.h"

struct StaticQueue_t final {
    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable space;
    uint8_t* buffer{};
    size_t capacity{};
    size_t item_size{};
    size_t head{};
    size_t count{};
};

using QueueHandle_t = StaticQueue_t*;

inline QueueHandle_t xQueueCreateStatic(UBaseType_t capacity, UBaseType_t item_size, uint8_t* buffer,
                                        StaticQueue_t* storage) {
    if (storage == nullptr || buffer == nullptr || capacity == 0U || item_size == 0U) {
        return nullptr;
    }
    storage->buffer = buffer;
    storage->capacity = capacity;
    storage->item_size = item_size;
    storage->head = 0U;
    storage->count = 0U;
    return storage;
}

inline BaseType_t xQueueReset(QueueHandle_t queue) {
    if (queue == nullptr) {
        return pdFALSE;
    }
    {
        std::lock_guard lock(queue->mutex);
        queue->head = 0U;
        queue->count = 0U;
    }
    queue->space.notify_all();
    return pdTRUE;
}

inline BaseType_t xQueueOverwrite(QueueHandle_t queue, const void* item) {
    if (queue == nullptr || item == nullptr || queue->capacity != 1U) {
        return pdFALSE;
    }
    {
        std::lock_guard lock(queue->mutex);
        std::memcpy(queue->buffer, item, queue->item_size);
        queue->head = 0U;
        queue->count = 1U;
    }
    queue->ready.notify_all();
    return pdTRUE;
}

inline BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t timeout) {
    if (queue == nullptr || item == nullptr) {
        return pdFALSE;
    }
    std::unique_lock lock(queue->mutex);
    const auto has_space = [queue] { return queue->count < queue->capacity; };
    if (timeout == portMAX_DELAY) {
        queue->space.wait(lock, has_space);
    } else if (timeout == 0U) {
        if (!has_space()) {
            return pdFALSE;
        }
    } else if (!queue->space.wait_for(lock, std::chrono::milliseconds(timeout), has_space)) {
        return pdFALSE;
    }

    const size_t tail = (queue->head + queue->count) % queue->capacity;
    std::memcpy(queue->buffer + (tail * queue->item_size), item, queue->item_size);
    ++queue->count;
    lock.unlock();
    queue->ready.notify_all();
    return pdTRUE;
}

inline BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t timeout) {
    if (queue == nullptr || item == nullptr) {
        return pdFALSE;
    }
    std::unique_lock lock(queue->mutex);
    if (timeout == portMAX_DELAY) {
        queue->ready.wait(lock, [queue] { return queue->count != 0U; });
    } else if (timeout == 0U) {
        if (queue->count == 0U) {
            return pdFALSE;
        }
    } else if (!queue->ready.wait_for(lock, std::chrono::milliseconds(timeout),
                                      [queue] { return queue->count != 0U; })) {
        return pdFALSE;
    }
    std::memcpy(item, queue->buffer + (queue->head * queue->item_size), queue->item_size);
    queue->head = (queue->head + 1U) % queue->capacity;
    --queue->count;
    lock.unlock();
    queue->space.notify_all();
    return pdTRUE;
}

#endif
