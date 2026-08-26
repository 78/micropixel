#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_I2C_EXECUTOR_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_I2C_EXECUTOR_HPP

#include <array>
#include <atomic>
#include <cstdint>

#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace micropixel::platform::metalio_claw4 {

// One executor owns all runtime transactions on the board's physical I2C bus.
// Drivers keep their own state machines; they submit only bounded bus work.
class I2cExecutor final {
   public:
    enum class Priority : uint8_t {
        kHigh,
        kNormal,
        kLow,
    };

    using Operation = esp_err_t (*)(void* context);

    I2cExecutor() = default;
    I2cExecutor(const I2cExecutor&) = delete;
    I2cExecutor& operator=(const I2cExecutor&) = delete;
    ~I2cExecutor();

    [[nodiscard]] esp_err_t Initialize();
    [[nodiscard]] esp_err_t Invoke(Priority priority, Operation operation, void* context);
    [[nodiscard]] bool Post(Priority priority, Operation operation, void* context);
    [[nodiscard]] bool PostFromIsr(Priority priority, Operation operation, void* context,
                                   BaseType_t* higher_priority_task_woken);

   private:
    struct Completion final {
        SemaphoreHandle_t ready{};
        esp_err_t result{ESP_FAIL};
    };

    struct Job final {
        Operation operation{};
        void* context{};
        Completion* completion{};
    };

    static constexpr uint32_t kQueueCapacity = 8U;
    static constexpr uint32_t kWorkerStackBytes = 4096U;
    static constexpr BaseType_t kWorkerCore = 1;

    static void WorkerEntry(void* context);
    void WorkerLoop();
    [[nodiscard]] QueueHandle_t Queue(Priority priority) const;
    [[nodiscard]] bool Receive(Job& job_out);
    void Stop();

    std::array<StaticQueue_t, 3> queue_storage_{};
    std::array<std::array<uint8_t, sizeof(Job) * kQueueCapacity>, 3> queue_bytes_{};
    std::array<QueueHandle_t, 3> queues_{};
    Completion completion_{};
    SemaphoreHandle_t invoke_mutex_{};
    SemaphoreHandle_t worker_stopped_{};
    std::atomic<TaskHandle_t> worker_{};
    std::atomic<bool> stopping_{};
};

}  // namespace micropixel::platform::metalio_claw4

#endif
