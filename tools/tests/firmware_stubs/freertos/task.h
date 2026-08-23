#ifndef MICROPIXEL_TEST_STUB_FREERTOS_TASK_H
#define MICROPIXEL_TEST_STUB_FREERTOS_TASK_H

#include <chrono>
#include <thread>

#include "freertos/FreeRTOS.h"

inline void vTaskDelay(TickType_t ticks) { std::this_thread::sleep_for(std::chrono::milliseconds(ticks)); }

inline TickType_t xTaskGetTickCount() {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<TickType_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

#endif
