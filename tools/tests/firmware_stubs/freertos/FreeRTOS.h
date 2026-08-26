#ifndef MICROPIXEL_TEST_STUB_FREERTOS_H
#define MICROPIXEL_TEST_STUB_FREERTOS_H

#include <cstdint>

using TickType_t = uint32_t;
using BaseType_t = int;
using UBaseType_t = unsigned int;

constexpr UBaseType_t configMAX_PRIORITIES = 25U;
constexpr uint32_t configTICK_RATE_HZ = 1000U;

constexpr BaseType_t pdFALSE = 0;
constexpr BaseType_t pdTRUE = 1;
constexpr BaseType_t pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr uint32_t portTICK_PERIOD_MS = 1U;

#define pdMS_TO_TICKS(milliseconds) static_cast<TickType_t>(milliseconds)

struct portMUX_TYPE final {
    uint32_t unused{};
};

#define portMUX_INITIALIZER_UNLOCKED \
    {                                \
    }
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define portYIELD_FROM_ISR() ((void)0)

#endif
