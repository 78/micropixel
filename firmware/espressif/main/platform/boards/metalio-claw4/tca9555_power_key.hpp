// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_TCA9555_POWER_KEY_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_TCA9555_POWER_KEY_HPP

#include <atomic>
#include <cstdint>

#include "button_interface.h"
#include "button_types.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "platform/buses/i2c_executor.hpp"

namespace micropixel::platform::metalio_claw4 {

class Tca9555PowerKey final {
   public:
    using PowerInputChangeSink = void (*)(void* context);
    using KeyPressSink = bool (*)(void* context, uint64_t timestamp_us);
    using PowerOffSink = bool (*)(void* context, uint64_t timestamp_us);

    [[nodiscard]] esp_err_t Initialize(i2c_master_dev_handle_t io_expander, buses::I2cExecutor& i2c_executor);
    void SetPowerInputChangeSink(PowerInputChangeSink sink, void* context);
    void SetKeyPressSink(KeyPressSink sink, void* context);
    void SetPowerOffSink(PowerOffSink sink, void* context);
    [[nodiscard]] esp_err_t PrepareForLightSleep();
    [[nodiscard]] bool IsPressed();
    [[nodiscard]] bool PowerPressOccurredAfterRequest() const;
    void GuardWakeButtonUntilRelease(uint64_t click_deadline_us);
    void SuppressSingleClicksUntil(uint64_t deadline_us);
    [[noreturn]] void PowerOff();

   private:
    struct DriverContext final {
        button_driver_t base{};
        Tca9555PowerKey* owner{};
    };

    [[nodiscard]] esp_err_t ConfigurePowerKeyInput();
    [[nodiscard]] esp_err_t ConfigureInterrupt();
    [[nodiscard]] esp_err_t ReadRegister(uint8_t address, uint8_t& value) const;
    [[nodiscard]] esp_err_t WriteRegister(uint8_t address, uint8_t value) const;
    [[nodiscard]] esp_err_t ReadPorts(uint8_t& port0, uint8_t& port1) const;
    [[nodiscard]] uint8_t ReadKeyLevel();
    [[nodiscard]] uint8_t RecordRawLevel(uint8_t port0, uint8_t port1, int interrupt_level);
    [[nodiscard]] esp_err_t EnterPowerSave();
    [[nodiscard]] esp_err_t ExitPowerSave();

    static uint8_t GetKeyLevel(button_driver_t* driver);
    static esp_err_t EnterPowerSaveCallback(button_driver_t* driver);
    static esp_err_t ExitPowerSaveCallback(button_driver_t* driver);
    static int32_t GetInterruptGpio(button_driver_t* driver);
    static esp_err_t DeleteDriver(button_driver_t* driver);
    static void IRAM_ATTR InterruptEntry(void* context);
    static void ButtonEvent(void* button, void* context);

    DriverContext driver_{};
    i2c_master_dev_handle_t io_expander_{};
    buses::I2cExecutor* i2c_executor_{};
    button_handle_t button_{};
    uint32_t read_error_count_{};
    uint8_t last_key_level_{BUTTON_INACTIVE};
    uint8_t last_power_inputs_{};
    PowerInputChangeSink power_input_change_sink_{};
    void* power_input_change_context_{};
    portMUX_TYPE key_press_sink_lock_ = portMUX_INITIALIZER_UNLOCKED;
    KeyPressSink key_press_sink_{};
    void* key_press_context_{};
    PowerOffSink power_off_sink_{};
    void* power_off_context_{};
    std::atomic<uint64_t> suppress_single_click_until_us_{};
    std::atomic<uint32_t> press_generation_{};
    std::atomic<uint32_t> power_request_generation_{};
    std::atomic_bool wake_key_release_required_{};
    bool raw_level_known_{};
    bool power_inputs_known_{};
    bool isr_registered_{};
    bool initialized_{};
};

}  // namespace micropixel::platform::metalio_claw4

#endif
