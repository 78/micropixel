#ifndef MICROPIXEL_WORK_BACKGROUND_EXECUTOR_HPP
#define MICROPIXEL_WORK_BACKGROUND_EXECUTOR_HPP

#include <array>
#include <atomic>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace micropixel::work {

// Process-wide, bounded executor for non-real-time Host work. Jobs run to
// completion on one low-priority Core 0 task; callers retain ownership of the
// job context until their completion protocol says otherwise.
class BackgroundExecutor final {
   public:
    using Function = void (*)(void* context);

    BackgroundExecutor();
    BackgroundExecutor(const BackgroundExecutor&) = delete;
    BackgroundExecutor& operator=(const BackgroundExecutor&) = delete;
    ~BackgroundExecutor();

    [[nodiscard]] bool valid() const;  // NOLINT(readability-identifier-naming)
    [[nodiscard]] bool Submit(Function function, void* context);
    void Shutdown();

   private:
    struct Job final {
        Function function{};
        void* context{};
    };

    static void WorkerEntry(void* context);
    void WorkerLoop();

    static constexpr uint32_t kQueueCapacity = 12U;

    StaticQueue_t queue_storage_{};
    std::array<uint8_t, sizeof(Job) * kQueueCapacity> queue_bytes_{};
    StaticSemaphore_t stopped_storage_{};
    QueueHandle_t queue_{};
    SemaphoreHandle_t stopped_{};
    TaskHandle_t worker_{};
    std::atomic<bool> stopping_{};
};

}  // namespace micropixel::work

#endif
