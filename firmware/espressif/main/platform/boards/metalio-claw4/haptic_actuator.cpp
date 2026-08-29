#include "platform/boards/metalio-claw4/haptic_actuator.hpp"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "platform/boards/metalio-claw4/board_config.hpp"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr ledc_timer_bit_t kResolution = LEDC_TIMER_10_BIT;
constexpr uint32_t kFrequencyHz = 5000U;
constexpr uint32_t kMaximumDuty = (1U << 10U) - 1U;
constexpr uint32_t kSafeMaximumDutyPerMille = 600U;

class LedcHapticActuator final : public haptics::HapticActuator {
   public:
    [[nodiscard]] esp_err_t Initialize() override {
        ledc_timer_config_t timer{};
        timer.speed_mode = board::kHapticLedcMode;
        timer.duty_resolution = kResolution;
        timer.timer_num = board::kHapticLedcTimer;
        timer.freq_hz = kFrequencyHz;
        timer.clk_cfg = LEDC_AUTO_CLK;
        esp_err_t status = ledc_timer_config(&timer);
        if (status != ESP_OK) {
            return status;
        }

        ledc_channel_config_t channel{};
        channel.gpio_num = board::kHapticMotor;
        channel.speed_mode = board::kHapticLedcMode;
        channel.channel = board::kHapticLedcChannel;
        channel.timer_sel = board::kHapticLedcTimer;
        channel.duty = 0U;
        return ledc_channel_config(&channel);
    }

    [[nodiscard]] esp_err_t SetStrength(uint16_t strength_per_mille) override {
        const uint32_t duty = kMaximumDuty * strength_per_mille * kSafeMaximumDutyPerMille / 1000000U;
        const esp_err_t status = ledc_set_duty(board::kHapticLedcMode, board::kHapticLedcChannel, duty);
        return status == ESP_OK ? ledc_update_duty(board::kHapticLedcMode, board::kHapticLedcChannel) : status;
    }
};

}  // namespace

haptics::HapticActuator& ConfiguredHapticActuator() {
    static LedcHapticActuator actuator;
    return actuator;
}

}  // namespace micropixel::platform::metalio_claw4
