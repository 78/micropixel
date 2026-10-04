// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>

#include "device/contracts/power.hpp"

namespace micropixel::platform::m5stack_tab5 {

class BoardHardware;

// Tab5 exposes no power key to the SoC: the board's Reset/Power key is handled
// by an independent MCU outside this firmware, and neither the touch interrupt
// nor any other line is routed to an SoC wake source. The contract therefore
// rules out light sleep ("boards without a wake source must use power off or
// disable the timeout"), so the idle action is a power off implemented as a
// deep sleep: the screen goes dark and the reset button brings the board back
// through an ordinary cold boot.
class Tab5PowerController final : public device::Power {
   public:
    explicit Tab5PowerController(BoardHardware& hardware) : hardware_(hardware) {}

    void SetPowerButtonSink(device::PowerButtonSink /*sink*/, void* /*context*/) override {}
    void SetPowerOffButtonSink(device::PowerOffButtonSink /*sink*/, void* /*context*/) override {}

    [[nodiscard]] device::IdlePowerAction GetIdlePowerAction() const override {
        return device::IdlePowerAction::kPowerOff;
    }
    // Light sleep stays rejected on this board; see the class comment.
    [[nodiscard]] std::expected<void, device::PowerError> EnterLowPower() override {
        return std::unexpected(device::PowerError::kSleepRejected);
    }
    [[noreturn]] void PowerOff() override;

   private:
    BoardHardware& hardware_;
};

}  // namespace micropixel::platform::m5stack_tab5
