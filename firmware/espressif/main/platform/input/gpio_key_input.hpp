#pragma once

#include <atomic>
#include <cstdint>

#include "device/contracts/input.hpp"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace micropixel::platform::input {

struct GpioKeyInputConfig final {
    gpio_num_t pin;
    device::KeyCode code;
    const char* log_tag;
    const char* task_name;
    BaseType_t task_core;
    uint32_t debounce_ms{20U};
    bool active_low{true};
};

class GpioKeyInput final {
   public:
    explicit GpioKeyInput(GpioKeyInputConfig config) : config_(config) {}
    GpioKeyInput(const GpioKeyInput&) = delete;
    GpioKeyInput& operator=(const GpioKeyInput&) = delete;
    ~GpioKeyInput();

    [[nodiscard]] esp_err_t Initialize(device::Input& input);

   private:
    struct EdgeRecord final {
        uint64_t timestamp_us{};
    };

    [[nodiscard]] bool IsPressed() const;
    static void IRAM_ATTR OnEdge(void* context);
    static void WorkerEntry(void* context);
    void Worker();
    void Stop();

    GpioKeyInputConfig config_;
    device::Input* input_{};
    QueueHandle_t edge_queue_{};
    SemaphoreHandle_t worker_stopped_{};
    TaskHandle_t worker_{};
    std::atomic<bool> stopping_{};
    bool pressed_{};
    bool isr_registered_{};
};

}  // namespace micropixel::platform::input
