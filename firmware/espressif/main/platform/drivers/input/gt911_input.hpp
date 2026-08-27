#ifndef MICROPIXEL_PLATFORM_DRIVERS_INPUT_GT911_INPUT_HPP
#define MICROPIXEL_PLATFORM_DRIVERS_INPUT_GT911_INPUT_HPP

#include <atomic>
#include <cstdint>

#include "device/input.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "platform/common/i2c_executor.hpp"

namespace micropixel::platform::drivers {

class Gt911Input final : public device::InputBackend {
   public:
    Gt911Input(int32_t width, int32_t height, gpio_num_t interrupt_pin);

    [[nodiscard]] esp_err_t Initialize(i2c_master_bus_handle_t i2c_bus, common::I2cExecutor& i2c_executor);
    [[nodiscard]] esp_err_t Start(lv_display_t* display);
    [[nodiscard]] bool Available() const { return touch_ != nullptr; }

    [[nodiscard]] int32_t GetInfo(micropixel_input_info_t& info) override;
    void BindTouchSink(device::TouchSink sink, void* context) override;
    void UnbindTouchSink(void* context) override;
    [[nodiscard]] bool InjectTouch(const device::TouchSample& sample) override;
    void BindSmokeUi(lv_obj_t* root, lv_obj_t* marker, lv_obj_t* status);
    void ClearSmokeUi();
    void InjectTouchForCapture(const device::TouchSample& sample);

   private:
    struct ActiveTouch final {
        bool active{};
        esp_lcd_touch_point_data_t point{};
    };

    [[nodiscard]] uint8_t ProbeAddress(i2c_master_bus_handle_t i2c_bus) const;
    [[nodiscard]] esp_err_t InitializeOnWorker(i2c_master_bus_handle_t i2c_bus);
    void Emit(const device::TouchSample& sample);
    void ProcessInterrupt();
    void UpdateSmokeUi(const esp_lcd_touch_point_data_t* points, uint8_t point_count);
    static void IRAM_ATTR InterruptEntry(esp_lcd_touch_handle_t touch);
    static esp_err_t ProcessEntry(void* context);

    static Gt911Input* active_instance;

    int32_t width_{};
    int32_t height_{};
    gpio_num_t interrupt_pin_{GPIO_NUM_NC};
    esp_lcd_touch_handle_t touch_{};
    lv_display_t* display_{};
    common::I2cExecutor* i2c_executor_{};
    std::atomic<uint32_t> interrupts_{};
    std::atomic<bool> work_pending_{};
    portMUX_TYPE sink_lock_ = portMUX_INITIALIZER_UNLOCKED;
    device::TouchSink sink_{};
    void* sink_context_{};
    uint32_t sink_inflight_{};
    ActiveTouch active_touches_[MICROPIXEL_MAX_TOUCH_POINTS]{};
    lv_obj_t* smoke_root_{};
    lv_obj_t* smoke_marker_{};
    lv_obj_t* smoke_status_{};
};

}  // namespace micropixel::platform::drivers

#endif
