#ifndef MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_POWER_CONTROLLER_HPP
#define MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_POWER_CONTROLLER_HPP

#include <atomic>
#include <cstdint>

#include "device/contracts/power.hpp"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "platform/audio/audio_power_controller.hpp"

namespace micropixel::platform::esp_mosaico {

class PowerController final : public audio::AudioPowerController {
   public:
    ~PowerController() override;

    [[nodiscard]] esp_err_t Initialize();
    [[nodiscard]] esp_err_t SetPeripheralPower(bool enabled);

    [[nodiscard]] esp_err_t PowerOnAudio() override;
    [[nodiscard]] esp_err_t PowerOffAudio() override;

    void SetPowerButtonSink(device::PowerButtonSink sink, void* context);
    void SetPowerOffButtonSink(device::PowerOffButtonSink sink, void* context);
    [[nodiscard]] esp_err_t PrepareForLightSleep();
    void FinishLightSleep();
    [[nodiscard]] bool IsPressed() const;
    [[nodiscard]] bool PowerPressOccurredAfterRequest() const;
    void GuardWakeButtonUntilRelease(uint64_t click_deadline_us);
    [[noreturn]] void PowerOff();

   private:
    enum class EventKind : uint8_t {
        kEdge,
        kLongPress,
        kStop,
    };

    struct Event final {
        EventKind kind{};
        uint64_t timestamp_us{};
    };

    [[nodiscard]] esp_err_t SetCodecPower(bool enabled);
    static void IRAM_ATTR OnEdge(void* context);
    static void LongPressTimer(void* context);
    static void WorkerEntry(void* context);
    void Worker();
    void HandleStableLevel(bool pressed, uint64_t timestamp_us);
    void Stop();

    QueueHandle_t event_queue_{};
    SemaphoreHandle_t worker_stopped_{};
    TaskHandle_t worker_{};
    esp_timer_handle_t long_press_timer_{};
    portMUX_TYPE sink_lock_ = portMUX_INITIALIZER_UNLOCKED;
    device::PowerButtonSink power_button_sink_{};
    void* power_button_context_{};
    device::PowerOffButtonSink power_off_sink_{};
    void* power_off_context_{};
    std::atomic<bool> stopping_{};
    std::atomic<bool> wake_release_required_{};
    std::atomic<uint64_t> suppress_click_until_us_{};
    std::atomic<uint32_t> press_generation_{};
    std::atomic<uint32_t> request_generation_{};
    uint64_t press_started_us_{};
    bool pressed_{};
    bool long_press_sent_{};
    bool isr_registered_{};
    bool initialized_{};
    bool peripheral_power_enabled_{};
    bool codec_power_enabled_{};
};

}  // namespace micropixel::platform::esp_mosaico

#endif
