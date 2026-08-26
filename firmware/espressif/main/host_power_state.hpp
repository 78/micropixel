#ifndef MICROPIXEL_FIRMWARE_HOST_POWER_STATE_HPP
#define MICROPIXEL_FIRMWARE_HOST_POWER_STATE_HPP

#include <atomic>
#include <cstdint>

namespace micropixel::firmware {

enum class HostPowerState : uint8_t {
    kAwake,
    kEnteringSleep,
    kAsleep,
    kWaking,
    kShuttingDown,
};

// Host supervisor owns this state machine. It deliberately does not encode the
// Hall/App scene or App lifecycle: those are orthogonal states with explicit
// invariants enforced by HostController during each transition.
class HostPowerStateMachine final {
   public:
    [[nodiscard]] HostPowerState state() const {  // NOLINT(readability-identifier-naming)
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool BeginSleep() {
        HostPowerState expected = HostPowerState::kAwake;
        return state_.compare_exchange_strong(expected, HostPowerState::kEnteringSleep, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool MarkAsleep() {
        HostPowerState expected = HostPowerState::kEnteringSleep;
        return state_.compare_exchange_strong(expected, HostPowerState::kAsleep, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool BeginWake() {
        HostPowerState expected = HostPowerState::kAsleep;
        return state_.compare_exchange_strong(expected, HostPowerState::kWaking, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool FinishWake() {
        HostPowerState expected = HostPowerState::kWaking;
        return state_.compare_exchange_strong(expected, HostPowerState::kAwake, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool BeginShutdown() {
        HostPowerState expected = HostPowerState::kAwake;
        return state_.compare_exchange_strong(expected, HostPowerState::kShuttingDown, std::memory_order_acq_rel);
    }

    void RecoverAwake() { state_.store(HostPowerState::kAwake, std::memory_order_release); }

   private:
    std::atomic<HostPowerState> state_{HostPowerState::kAwake};
};

}  // namespace micropixel::firmware

#endif
