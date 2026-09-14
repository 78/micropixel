#include <cassert>
#include <cstdlib>
#include <iostream>

#include "host/controller/hall_battery_policy.hpp"
#include "host/controller/host_power_state.hpp"
#include "platform/boards/esp-mosaico/battery_power_policy.hpp"
#include "platform/boards/esp-mosaico/battery_profile.hpp"
#include "platform/boards/esp-mosaico/hardware_config.hpp"

namespace {

using micropixel::firmware::HostPowerState;
using micropixel::firmware::HostPowerStateMachine;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void CompleteCycleIsStrictlyOrdered() {
    HostPowerStateMachine state;
    Check(state.state() == HostPowerState::kAwake, "power state should start awake");
    Check(state.BeginSleep(), "awake state should begin sleep");
    Check(state.state() == HostPowerState::kEnteringSleep, "sleep preparation state should be explicit");
    Check(state.MarkAsleep(), "prepared state should enter asleep");
    Check(state.BeginWake(), "asleep state should begin wake");
    Check(state.state() == HostPowerState::kWaking, "wake preparation state should be explicit");
    Check(state.FinishWake(), "waking state should finish awake");
    Check(state.state() == HostPowerState::kAwake, "complete cycle should end awake");
}

void InvalidTransitionsDoNotMutateState() {
    HostPowerStateMachine state;
    Check(!state.MarkAsleep(), "awake state must not skip sleep preparation");
    Check(!state.BeginWake(), "awake state must not begin wake");
    Check(!state.FinishWake(), "awake state must not finish wake");
    Check(state.BeginSleep(), "valid transition should remain possible after rejected transitions");
    Check(!state.BeginSleep(), "entering-sleep state must reject a second sleep request");
    Check(state.state() == HostPowerState::kEnteringSleep, "rejected transitions must not change state");
}

void RecoveryReturnsToAwake() {
    HostPowerStateMachine state;
    Check(state.BeginSleep(), "test setup should begin sleep");
    Check(state.MarkAsleep(), "test setup should enter asleep");
    state.RecoverAwake();
    Check(state.state() == HostPowerState::kAwake, "error recovery should restore the stable awake state");
    Check(state.BeginSleep(), "a recovered state should accept a later power cycle");
}

void ShutdownIsTerminalFromAwakeOnly() {
    HostPowerStateMachine state;
    Check(state.BeginShutdown(), "awake state should accept shutdown");
    Check(state.state() == HostPowerState::kShuttingDown, "shutdown state should be explicit");
    Check(!state.BeginSleep(), "shutdown state must reject sleep");
    Check(!state.BeginWake(), "shutdown state must reject wake");
    Check(!state.BeginShutdown(), "shutdown state must reject duplicate shutdown");
}

void TestMosaicoBatteryPowerPolicy() {
    namespace battery_profile = micropixel::platform::esp_mosaico::battery_profile;
    namespace drivers = micropixel::platform::drivers;
    using namespace micropixel::platform::esp_mosaico::battery_policy;

    static_assert(IsCharging(6, false));
    static_assert(!IsCharging(6, true));
    static_assert(!IsCharging(5, false));
    static_assert(IsDischarging(-6));
    static_assert(!IsDischarging(-5));
    static_assert(!ExternalPowerConnected(-6));
    static_assert(ExternalPowerConnected(-5));
    static_assert(ExternalPowerConnected(0));
    static_assert(ExternalPowerConnected(6));
    static_assert(battery_profile::kProfile.design_capacity_mah == 80U);
    static_assert(battery_profile::kParameters.size() == 26U);
    static_assert(battery_profile::kParameters[4].width == drivers::Bq27220DataWidth::kU8);
    constexpr std::array<uint8_t, 4U> profile_write{0x9fU, 0x92U, 0x00U, 0x50U};
    static_assert(drivers::Bq27220Checksum(profile_write) == 0x7eU);
}

void TestMosaicoHardwareRevisions() {
    using micropixel::platform::esp_mosaico::board::DecodeHardwareConfig;
    constexpr auto legacy = DecodeHardwareConfig(0x0100U);
    static_assert(legacy && legacy->display_reset == 42 && legacy->display_clock == 44);
    static_assert(legacy->i2c_data == 0 && legacy->i2c_clock == 1);
    static_assert(legacy->codec_power == 56 && legacy->status_led == 3);
    for (uint32_t version = 0U; version <= 0xffffU; ++version) {
        const auto config = DecodeHardwareConfig(static_cast<uint16_t>(version));
        Check(config.has_value() == (version >= 0x0100U && version <= 0x0102U),
              "only documented eFuse revisions may configure hardware");
        if (!config) {
            continue;
        }
        Check(config->display_reset != config->display_clock, "LCD reset must not share the QSPI clock");
        Check(config->codec_power != config->i2c_data && config->status_led != config->i2c_clock,
              "power and LED outputs must not drive onboard I2C pins");
    }
    for (const uint16_t version : {0x0101U, 0x0102U}) {
        const auto config = DecodeHardwareConfig(version);
        Check(config && config->display_reset == 44 && config->display_clock == 42,
              "v1.1/v1.2 must swap LCD reset and clock");
        Check(config->i2c_data == 56 && config->i2c_clock == 3, "v1.1/v1.2 must move all onboard I2C");
        Check(config->codec_power == -1 && config->status_led == -1,
              "v1.1/v1.2 must remove codec rail and LED controls");
    }
}

void TestHallBatteryPolicy() {
    using micropixel::firmware::hall_battery_policy::ShowCharging;

    static_assert(ShowCharging(true, true, true, true));
    static_assert(!ShowCharging(true, false, true, true));
    static_assert(!ShowCharging(true, false, true, false));
    static_assert(ShowCharging(false, false, true, true));
    static_assert(!ShowCharging(false, false, false, false));
}

}  // namespace

int main() {
    CompleteCycleIsStrictlyOrdered();
    InvalidTransitionsDoNotMutateState();
    RecoveryReturnsToAwake();
    ShutdownIsTerminalFromAwakeOnly();
    TestMosaicoBatteryPowerPolicy();
    TestHallBatteryPolicy();
    TestMosaicoHardwareRevisions();
    return 0;
}
