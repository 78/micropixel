#ifndef MICROPIXEL_PLATFORM_HAPTICS_TIMED_HAPTICS_PERIPHERAL_HPP
#define MICROPIXEL_PLATFORM_HAPTICS_TIMED_HAPTICS_PERIPHERAL_HPP

#include <atomic>
#include <cstdint>

#include "device/contracts/haptics.hpp"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace micropixel::platform::haptics {

class HapticActuator {
   public:
    virtual ~HapticActuator() = default;
    HapticActuator(const HapticActuator&) = delete;
    HapticActuator& operator=(const HapticActuator&) = delete;

    [[nodiscard]] virtual esp_err_t Initialize() = 0;
    [[nodiscard]] virtual esp_err_t SetStrength(uint16_t strength_per_mille) = 0;

   protected:
    HapticActuator() = default;
};

class TimedHapticsPeripheral final : public device::HapticsPeripheral {
   public:
    static constexpr device::PeripheralChannelId kChannel = 1U;

    TimedHapticsPeripheral(HapticActuator& actuator, uint32_t capabilities, uint32_t maximum_duration_ms = 5000U);
    ~TimedHapticsPeripheral() override;

    [[nodiscard]] esp_err_t Initialize();
    [[nodiscard]] int32_t GetInfo(device::PeripheralChannelId channel,
                                  micropixel_haptics_info_t& info_out) const override;
    [[nodiscard]] int32_t Play(device::PeripheralChannelId channel, uint16_t strength_per_mille,
                               uint32_t duration_ms) override;
    [[nodiscard]] int32_t Stop(device::PeripheralChannelId channel) override;
    void SetCompletionSink(device::HapticsPeripheralCompletionSink sink, void* context) override;

   private:
    static void OnFinished(void* context);
    [[nodiscard]] esp_err_t SetStrength(uint16_t strength_per_mille) const;

    HapticActuator& actuator_;
    uint32_t capabilities_{};
    uint32_t maximum_duration_ms_{};
    esp_timer_handle_t stop_timer_{};
    SemaphoreHandle_t callback_mutex_{};
    std::atomic<device::HapticsPeripheralCompletionSink> completion_sink_{};
    std::atomic<void*> completion_context_{};
    std::atomic<bool> playing_{};
};

}  // namespace micropixel::platform::haptics

#endif
