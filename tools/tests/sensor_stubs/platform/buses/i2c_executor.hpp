// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <deque>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace micropixel::platform::buses {
// Deterministic priority/FIFO scheduling, including queued work at a barrier.
class I2cExecutor final {
   public:
    enum class Priority { kHigh, kNormal, kLow };
    using Operation = esp_err_t (*)(void*);
    struct Job {
        Priority priority;
        Operation operation;
        void* context;
    };
    bool reject_post{};
    bool reject_invoke{};
    uint32_t busy_low_invokes{};
    bool on_worker{};
    std::deque<Job> jobs;

    bool Post(Priority priority, Operation operation, void* context) {
        if (reject_post) return false;
        jobs.push_back({priority, operation, context});
        return true;
    }
    void Drain(Priority priority = Priority::kLow) {
        for (;;) {
            auto selected = jobs.end();
            for (auto it = jobs.begin(); it != jobs.end(); ++it) {
                if (it->priority <= priority && (selected == jobs.end() || it->priority < selected->priority)) {
                    selected = it;
                }
            }
            if (selected == jobs.end()) return;
            const Job job = *selected;
            jobs.erase(selected);
            on_worker = true;
            (void)job.operation(job.context);
            on_worker = false;
        }
    }
    esp_err_t Invoke(Priority priority, Operation operation, void* context) {
        if (reject_invoke) return ESP_FAIL;
        if (priority == Priority::kLow && busy_low_invokes != 0U) {
            --busy_low_invokes;
            return ESP_ERR_NO_MEM;
        }
        Drain(priority);
        on_worker = true;
        const esp_err_t result = operation(context);
        on_worker = false;
        return result;
    }
};
}  // namespace micropixel::platform::buses
