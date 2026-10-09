// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <deque>

#include "esp_err.h"

namespace micropixel::platform::buses {

// Deterministic stand-in for the board's I2C executor. Invoke runs the operation
// inline, Post only queues it: a board test can therefore drive the bring-up
// sync and the periodic refresh without a worker task or real timing.
class I2cExecutor final {
   public:
    enum class Priority { kHigh, kNormal, kLow };
    using Operation = esp_err_t (*)(void*);

    struct Job final {
        Priority priority;
        Operation operation;
        void* context;
    };

    bool reject_post{};
    bool reject_invoke{};
    bool on_worker{};
    std::deque<Job> jobs;

    [[nodiscard]] bool Post(Priority priority, Operation operation, void* context) {
        if (reject_post) {
            return false;
        }
        jobs.push_back({priority, operation, context});
        return true;
    }

    // Run queued work in priority/FIFO order, lowest priority number first.
    void Drain(Priority priority = Priority::kLow) {
        for (;;) {
            auto selected = jobs.end();
            for (auto it = jobs.begin(); it != jobs.end(); ++it) {
                if (it->priority <= priority && (selected == jobs.end() || it->priority < selected->priority)) {
                    selected = it;
                }
            }
            if (selected == jobs.end()) {
                return;
            }
            const Job job = *selected;
            jobs.erase(selected);
            on_worker = true;
            (void)job.operation(job.context);
            on_worker = false;
        }
    }

    [[nodiscard]] esp_err_t Invoke(Priority priority, Operation operation, void* context) {
        if (reject_invoke || operation == nullptr) {
            return ESP_FAIL;
        }
        Drain(priority);
        on_worker = true;
        const esp_err_t result = operation(context);
        on_worker = false;
        return result;
    }
};

}  // namespace micropixel::platform::buses
