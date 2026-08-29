#include "platform/boards/esp-mosaico/haptic_actuator.hpp"

#include "driver/gpio.h"
#include "platform/boards/esp-mosaico/board_config.hpp"

namespace micropixel::platform::esp_mosaico {
namespace {

class GpioHapticActuator final : public haptics::HapticActuator {
   public:
    [[nodiscard]] esp_err_t Initialize() override {
        gpio_config_t config{};
        config.pin_bit_mask = BIT64(board::kHapticMotor);
        config.mode = GPIO_MODE_OUTPUT;
        config.pull_up_en = GPIO_PULLUP_DISABLE;
        config.pull_down_en = GPIO_PULLDOWN_ENABLE;
        config.intr_type = GPIO_INTR_DISABLE;
        return gpio_config(&config);
    }

    [[nodiscard]] esp_err_t SetStrength(uint16_t strength_per_mille) override {
        return gpio_set_level(board::kHapticMotor, strength_per_mille == 0U ? 0 : 1);
    }
};

}  // namespace

haptics::HapticActuator& ConfiguredHapticActuator() {
    static GpioHapticActuator hardware;
    return hardware;
}

}  // namespace micropixel::platform::esp_mosaico
