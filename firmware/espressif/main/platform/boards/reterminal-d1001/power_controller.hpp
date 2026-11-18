// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>

#include "device/contracts/power.hpp"

namespace micropixel::platform::reterminal_d1001 {

class BoardHardware;

// The D1001 latches its own 3V3 rail through PCA9535 P1.0, so the power-off
// path can cut the board instead of only stopping the SoC. Light sleep stays
// rejected until display and Wi-Fi retention have been validated on hardware.
class D1001PowerController final : public device::Power {
   public:
    explicit D1001PowerController(BoardHardware& hardware) : hardware_(hardware) {}

    void SetPowerButtonSink(device::PowerButtonSink sink, void* context) override {
        button_sink_ = sink;
        button_context_ = context;
    }
    void SetPowerOffButtonSink(device::PowerOffButtonSink sink, void* context) override {
        power_off_sink_ = sink;
        power_off_context_ = context;
    }

    [[nodiscard]] device::IdlePowerAction GetIdlePowerAction() const override {
        // Bring-up keeps the board awake: the power-key polarity and the rail
        // latch are not validated yet. The System menu can still power off.
        return device::IdlePowerAction::kDisabled;
    }
    [[nodiscard]] std::expected<void, device::PowerError> EnterLowPower() override {
        return std::unexpected(device::PowerError::kSleepRejected);
    }
    [[noreturn]] void PowerOff() override;

    // Called by the board's key input when the green button is released or held.
    [[nodiscard]] bool NotifyClick(uint64_t timestamp_us) {
        return button_sink_ != nullptr && button_sink_(button_context_, timestamp_us);
    }
    [[nodiscard]] bool NotifyLongPress(uint64_t timestamp_us) {
        return power_off_sink_ != nullptr && power_off_sink_(power_off_context_, timestamp_us);
    }

   private:
    BoardHardware& hardware_;
    device::PowerButtonSink button_sink_{};
    void* button_context_{};
    device::PowerOffButtonSink power_off_sink_{};
    void* power_off_context_{};
};

}  // namespace micropixel::platform::reterminal_d1001