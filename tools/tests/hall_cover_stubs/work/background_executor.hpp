// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <deque>
#include <utility>

namespace micropixel::work {
// Deterministic worker: submitting a job must never run it on the UI caller.
class BackgroundExecutor final {
   public:
    using Function = void (*)(void*);
    bool valid() const { return true; }
    bool Submit(Function function, void* context) {
        jobs_.emplace_back(function, context);
        return true;
    }
    void RunAll() {
        while (!jobs_.empty()) {
            const auto job = jobs_.front();
            jobs_.pop_front();
            job.first(job.second);
        }
    }

   private:
    std::deque<std::pair<Function, void*>> jobs_;
};
}  // namespace micropixel::work
