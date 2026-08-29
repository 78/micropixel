#ifndef MICROPIXEL_PLATFORM_INPUT_ESP_LCD_TOUCH_INPUT_HPP
#define MICROPIXEL_PLATFORM_INPUT_ESP_LCD_TOUCH_INPUT_HPP

#include <atomic>
#include <cstdint>

#include "device/contracts/input.hpp"
#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "platform/buses/i2c_executor.hpp"

namespace micropixel::platform::input {

// Adapts an interrupt-capable esp_lcd_touch controller to the hardware-neutral
// Input contract. The controller ISR only schedules fixed-capacity I2C
// work; all bus access and Guest/Host event delivery runs on the executor task.
class EspLcdTouchInput final : public device::Input {
   public:
    EspLcdTouchInput(int32_t width, int32_t height);

    [[nodiscard]] esp_err_t Initialize(esp_lcd_touch_handle_t touch, buses::I2cExecutor& executor);
    [[nodiscard]] esp_err_t Start(lv_display_t* display);
    [[nodiscard]] bool Available() const { return touch_ != nullptr; }

    [[nodiscard]] int32_t GetInfo(micropixel_input_info_t& info) override;
    void BindTouchSink(device::TouchSink sink, void* context) override;
    void UnbindTouchSink(void* context) override;
    [[nodiscard]] bool InjectTouch(const device::TouchSample& sample) override;

   private:
    struct ActiveTouch final {
        bool active{};
        esp_lcd_touch_point_data_t point{};
    };

    void Emit(const device::TouchSample& sample);
    void ProcessInterrupt();
    static esp_err_t PrimeEntry(void* context);
    static void IRAM_ATTR InterruptEntry(esp_lcd_touch_handle_t touch);
    static esp_err_t ProcessEntry(void* context);

    static EspLcdTouchInput* active_instance_;

    int32_t width_{};
    int32_t height_{};
    esp_lcd_touch_handle_t touch_{};
    lv_display_t* display_{};
    buses::I2cExecutor* executor_{};
    std::atomic<uint32_t> interrupts_{};
    std::atomic<bool> work_pending_{};
    portMUX_TYPE sink_lock_ = portMUX_INITIALIZER_UNLOCKED;
    device::TouchSink sink_{};
    void* sink_context_{};
    uint32_t sink_inflight_{};
    ActiveTouch active_touches_[MICROPIXEL_MAX_TOUCH_POINTS]{};
};

}  // namespace micropixel::platform::input

#endif
