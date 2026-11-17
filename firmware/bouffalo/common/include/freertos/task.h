// SPDX-License-Identifier: Apache-2.0
#pragma once
// FreeRTOS requires its configuration before the native task/queue headers.
// clang-format off
#include "freertos/FreeRTOS.h"
#include <task.h>
// clang-format on

#ifdef __cplusplus
extern "C" {
#endif
static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char *name, uint32_t stack_bytes,
                                                 void *argument, UBaseType_t priority, TaskHandle_t *task,
                                                 BaseType_t core) {
    (void)core;
    return xTaskCreate(function, name, (stack_bytes + sizeof(StackType_t) - 1U) / sizeof(StackType_t), argument,
                       priority, task);
}
static inline size_t mpx_stack_free_bytes(TaskHandle_t task) {
    return (size_t)uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t);
}
#define uxTaskGetStackHighWaterMark2 mpx_stack_free_bytes
#ifdef __cplusplus
}
#endif

#define xPortGetCoreID() 0
