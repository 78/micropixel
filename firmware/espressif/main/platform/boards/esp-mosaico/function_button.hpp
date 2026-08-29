#pragma once

#include <atomic>
#include <cstdint>

#include "device/contracts/input.hpp"
#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace micropixel::platform::esp_mosaico {

class FunctionButton final {
   public:
    FunctionButton() = default;
    FunctionButton(const FunctionButton&) = delete;
    FunctionButton& operator=(const FunctionButton&) = delete;
    ~FunctionButton();

    [[nodiscard]] esp_err_t Initialize(device::Input& input);

   private:
    struct EdgeRecord final {
        uint64_t timestamp_us{};
    };

    static void IRAM_ATTR OnEdge(void* context);
    static void WorkerEntry(void* context);
    void Worker();
    void Stop();

    device::Input* input_{};
    QueueHandle_t edge_queue_{};
    SemaphoreHandle_t worker_stopped_{};
    TaskHandle_t worker_{};
    std::atomic<bool> stopping_{};
    bool pressed_{};
    bool isr_registered_{};
};

}  // namespace micropixel::platform::esp_mosaico
