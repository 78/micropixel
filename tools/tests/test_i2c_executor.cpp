#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "platform/common/i2c_executor.hpp"

namespace {

using micropixel::platform::common::I2cExecutor;

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

struct Sequence final {
    std::atomic<bool> blocker_entered{};
    std::atomic<bool> release_blocker{};
    std::atomic<uint32_t> count{};
    uint32_t values[4]{};
};

esp_err_t Block(void* context) {
    auto& sequence = *static_cast<Sequence*>(context);
    sequence.blocker_entered.store(true);
    while (!sequence.release_blocker.load()) {
        std::this_thread::yield();
    }
    sequence.values[sequence.count.fetch_add(1U)] = 0U;
    return ESP_OK;
}

template <uint32_t Value>
esp_err_t Record(void* context) {
    auto& sequence = *static_cast<Sequence*>(context);
    sequence.values[sequence.count.fetch_add(1U)] = Value;
    return ESP_OK;
}

struct Nested final {
    I2cExecutor* executor;
    bool inner_called{};
};

esp_err_t InvokeNested(void* context) {
    auto& nested = *static_cast<Nested*>(context);
    return nested.executor->Invoke(
        I2cExecutor::Priority::kNormal,
        [](void* inner_context) {
            static_cast<Nested*>(inner_context)->inner_called = true;
            return ESP_OK;
        },
        &nested);
}

}  // namespace

int main() {
    I2cExecutor executor;
    Require(executor.Initialize() == ESP_OK);

    Sequence sequence;
    Require(executor.Post(I2cExecutor::Priority::kHigh, Block, &sequence));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (!sequence.blocker_entered.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    Require(sequence.blocker_entered.load());
    Require(executor.Post(I2cExecutor::Priority::kLow, Record<3U>, &sequence));
    Require(executor.Post(I2cExecutor::Priority::kNormal, Record<2U>, &sequence));
    Require(executor.Post(I2cExecutor::Priority::kHigh, Record<1U>, &sequence));
    sequence.release_blocker.store(true);
    while (sequence.count.load() != 4U && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    Require(sequence.count.load() == 4U);
    Require(sequence.values[0] == 0U && sequence.values[1] == 1U && sequence.values[2] == 2U &&
            sequence.values[3] == 3U);

    sequence.count.store(0U);
    std::atomic<uint32_t> invoke_successes{};
    std::array<std::thread, 4> callers;
    for (std::thread& caller : callers) {
        caller = std::thread([&executor, &sequence, &invoke_successes] {
            if (executor.Invoke(I2cExecutor::Priority::kNormal, Record<9U>, &sequence) == ESP_OK) {
                invoke_successes.fetch_add(1U);
            }
        });
    }
    for (std::thread& caller : callers) {
        caller.join();
    }
    Require(invoke_successes.load() == callers.size() && sequence.count.load() == callers.size());

    Nested nested{&executor};
    Require(executor.Invoke(I2cExecutor::Priority::kNormal, InvokeNested, &nested) == ESP_OK);
    Require(nested.inner_called);
    return 0;
}
