#include <cstdlib>
#include <iostream>

#include "host_power_state.hpp"

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

}  // namespace

int main() {
    CompleteCycleIsStrictlyOrdered();
    InvalidTransitionsDoNotMutateState();
    RecoveryReturnsToAwake();
    ShutdownIsTerminalFromAwakeOnly();
    std::cout << "host_power_state tests passed: 4 cases\n";
    return 0;
}
