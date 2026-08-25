// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_INPUT_TCA9555_POWER_KEY_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_INPUT_TCA9555_POWER_KEY_HPP

#include <cstdint>

#include "button_interface.h"
#include "button_types.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_err.h"

namespace micropixel::platform::metalio_claw4 {

class Tca9555PowerKey final {
   public:
    using PowerInputChangeSink = void (*)(void* context);

    [[nodiscard]] esp_err_t Initialize(i2c_master_dev_handle_t io_expander);
    void SetPowerInputChangeSink(PowerInputChangeSink sink, void* context);

   private:
    struct DriverContext final {
        button_driver_t base{};
        Tca9555PowerKey* owner{};
    };

    [[nodiscard]] esp_err_t ConfigurePowerKeyInput();
    [[nodiscard]] esp_err_t ConfigureInterrupt();
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
    button_handle_t button_{};
    uint32_t read_error_count_{};
    uint8_t last_key_level_{BUTTON_INACTIVE};
    uint8_t last_power_inputs_{};
    PowerInputChangeSink power_input_change_sink_{};
    void* power_input_change_context_{};
    bool raw_level_known_{};
    bool power_inputs_known_{};
    bool isr_registered_{};
    bool initialized_{};
};

}  // namespace micropixel::platform::metalio_claw4

#endif
