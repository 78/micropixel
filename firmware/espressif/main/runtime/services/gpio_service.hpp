#ifndef MICROPIXEL_RUNTIME_SERVICES_GPIO_SERVICE_HPP
#define MICROPIXEL_RUNTIME_SERVICES_GPIO_SERVICE_HPP

#include <atomic>

#include "device/device_services.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "runtime/event_queue.hpp"
#include "runtime/runtime_limits.hpp"
#include "runtime/services/service_result.hpp"

namespace micropixel::runtime {

class TimerService;

class GpioService final {
   public:
    GpioService(device::GpioService& gpio, EventQueue& events, TimerService& clock);
    GpioService(const GpioService&) = delete;
    GpioService& operator=(const GpioService&) = delete;
    ~GpioService();

    [[nodiscard]] bool valid() const { return mutex_ != nullptr; }  // NOLINT(readability-identifier-naming)
    [[nodiscard]] ServiceResult<micropixel_gpio_open_response_t> Open(const micropixel_gpio_open_request_t& request);
    [[nodiscard]] ServiceResult<micropixel_gpio_value_response_t> Read(micropixel_gpio_handle_t gpio);
    [[nodiscard]] ServiceResult<void> Write(micropixel_gpio_handle_t gpio, bool value);
    [[nodiscard]] ServiceResult<void> SetPwmDuty(micropixel_gpio_handle_t gpio, uint16_t duty_per_mille);
    [[nodiscard]] ServiceResult<void> Release(micropixel_gpio_handle_t gpio);
    void Suspend();
    [[nodiscard]] bool Resume();
    void Shutdown();

   private:
    struct Slot final {
        micropixel_device_id_t device{};
        micropixel_gpio_handle_t handle{};
        uint32_t generation{};
        uint32_t sequence{};
        uint16_t mode{};
        uint16_t edge{};
    };

    static void OnEdge(void* context, micropixel_device_id_t device, bool value, uint64_t timestamp_us);
    void HandleEdge(micropixel_device_id_t device, bool value);
    [[nodiscard]] Slot* Find(micropixel_gpio_handle_t handle);
    [[nodiscard]] bool TakeLock();
    void GiveLock();

    device::GpioService& gpio_;
    EventQueue& events_;
    TimerService& clock_;
    SemaphoreHandle_t mutex_{};
    Slot slots_[limits::kMaxGpioHandles]{};
    std::atomic<bool> suspended_{};
    bool shut_down_{};
};

}  // namespace micropixel::runtime

#endif
