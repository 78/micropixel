#ifndef MICROPIXEL_FIRMWARE_REMOTE_CONTROL_RECONNECT_POLICY_HPP
#define MICROPIXEL_FIRMWARE_REMOTE_CONTROL_RECONNECT_POLICY_HPP

#include <algorithm>
#include <cstdint>

namespace micropixel::firmware::remote_control {

constexpr bool ShouldResetIdentityForControlStatus(int status) { return status == 401; }

class ReconnectBackoff final {
   public:
    static constexpr uint32_t kInitialDelayCapMs = 10000U;
    static constexpr uint32_t kMaximumDelayCapMs = 5U * 60U * 1000U;

    [[nodiscard]] uint32_t NextDelayMs(uint32_t entropy) {
        const uint32_t delay_cap_ms = delay_cap_ms_;
        delay_cap_ms_ = std::min(delay_cap_ms_ * 2U, kMaximumDelayCapMs);

        // Equal jitter keeps every retry at least half of the current cap while
        // preventing a fleet of devices from reconnecting in lockstep.
        const uint32_t minimum_delay_ms = delay_cap_ms / 2U;
        const uint32_t jitter_range_ms = delay_cap_ms - minimum_delay_ms + 1U;
        return minimum_delay_ms + (entropy % jitter_range_ms);
    }

    void Reset() { delay_cap_ms_ = kInitialDelayCapMs; }

   private:
    uint32_t delay_cap_ms_{kInitialDelayCapMs};
};

}  // namespace micropixel::firmware::remote_control

#endif
