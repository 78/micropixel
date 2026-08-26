#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

#include "work/background_executor.hpp"

namespace {

struct BlockingJob final {
    std::mutex mutex;
    std::condition_variable condition;
    bool started{};
    bool released{};
};

void Block(void* context) {
    auto& job = *static_cast<BlockingJob*>(context);
    std::unique_lock lock(job.mutex);
    job.started = true;
    job.condition.notify_all();
    job.condition.wait(lock, [&job] { return job.released; });
}

struct OrderedJob final {
    std::mutex* mutex{};
    std::condition_variable* condition{};
    std::vector<uint32_t>* completed{};
    uint32_t value{};
};

void Record(void* context) {
    auto& job = *static_cast<OrderedJob*>(context);
    {
        std::lock_guard lock(*job.mutex);
        job.completed->push_back(job.value);
    }
    job.condition->notify_all();
}

}  // namespace

int main() {
    micropixel::work::BackgroundExecutor executor;
    assert(executor.valid());

    BlockingJob blocker;
    assert(executor.Submit(Block, &blocker));
    {
        std::unique_lock lock(blocker.mutex);
        assert(blocker.condition.wait_for(lock, std::chrono::seconds(1), [&blocker] { return blocker.started; }));
    }

    std::mutex completed_mutex;
    std::condition_variable completed_condition;
    std::vector<uint32_t> completed;
    std::vector<OrderedJob> jobs(13U);
    for (uint32_t index = 0U; index < 12U; ++index) {
        jobs[index] = {&completed_mutex, &completed_condition, &completed, index};
        assert(executor.Submit(Record, &jobs[index]));
    }
    jobs[12] = {&completed_mutex, &completed_condition, &completed, 12U};
    assert(!executor.Submit(Record, &jobs[12]));

    {
        std::lock_guard lock(blocker.mutex);
        blocker.released = true;
    }
    blocker.condition.notify_all();

    {
        std::unique_lock lock(completed_mutex);
        assert(completed_condition.wait_for(lock, std::chrono::seconds(1),
                                            [&completed] { return completed.size() == 12U; }));
        for (uint32_t index = 0U; index < completed.size(); ++index) {
            assert(completed[index] == index);
        }
    }

    executor.Shutdown();
    assert(!executor.Submit(Record, &jobs[0]));
    return 0;
}
