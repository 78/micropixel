#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_HAPTICS_BACKEND_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_HAPTICS_BACKEND_HPP

#include <atomic>

#include "device/haptics.hpp"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace micropixel::platform::metalio_claw4 {

class HapticsBackend final : public device::HapticsBackend {
   public:
    HapticsBackend() = default;
    ~HapticsBackend() override;

    [[nodiscard]] esp_err_t Initialize();
    [[nodiscard]] int32_t GetInfo(micropixel_device_id_t device, micropixel_haptics_info_t& info_out) const override;
    [[nodiscard]] int32_t Play(micropixel_device_id_t device, uint16_t strength_per_mille,
                               uint32_t duration_ms) override;
    [[nodiscard]] int32_t Stop(micropixel_device_id_t device) override;
    void SetCompletionSink(device::HapticCompletionSink sink, void* context) override;

   private:
    static void OnFinished(void* context);
    [[nodiscard]] esp_err_t ApplyStrength(uint16_t strength_per_mille);

    esp_timer_handle_t stop_timer_{};
    SemaphoreHandle_t callback_mutex_{};
    std::atomic<device::HapticCompletionSink> completion_sink_{};
    std::atomic<void*> completion_context_{};
    std::atomic<bool> playing_{};
};

}  // namespace micropixel::platform::metalio_claw4

#endif
