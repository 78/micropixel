#include <cassert>
#include <cstdlib>
#include <iostream>

#include "host/controller/hall_battery_policy.hpp"
#include "host/controller/host_power_state.hpp"
#include "platform/boards/esp-mosaico/battery_power_policy.hpp"
#include "platform/boards/esp-mosaico/battery_profile.hpp"

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
    return 0;
}
