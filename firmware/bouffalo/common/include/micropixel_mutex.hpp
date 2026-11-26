// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <mutex>

#include "freertos/semphr.h"
namespace micropixel::runtime {
class BouffaloMutex final {
   public:
    BouffaloMutex() : handle_(xSemaphoreCreateMutexStatic(&storage_)) { configASSERT(handle_); }
    void lock() { (void)xSemaphoreTake(handle_, portMAX_DELAY); }
    void unlock() { (void)xSemaphoreGive(handle_); }

   private:
    StaticSemaphore_t storage_{};
    SemaphoreHandle_t handle_{};
};
}  // namespace micropixel::runtime

using micropixel::runtime::BouffaloMutex;
