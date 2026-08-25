#include <cassert>
#include <cstdint>

#include "remote_control/reconnect_policy.hpp"

using micropixel::firmware::remote_control::ReconnectBackoff;
using micropixel::firmware::remote_control::ShouldResetIdentityForControlStatus;

int main() {
    assert(ShouldResetIdentityForControlStatus(401));
    assert(!ShouldResetIdentityForControlStatus(400));
    assert(!ShouldResetIdentityForControlStatus(403));
    assert(!ShouldResetIdentityForControlStatus(500));

    ReconnectBackoff backoff;
    constexpr uint32_t kCaps[] = {10000U, 20000U, 40000U, 80000U, 160000U, 300000U, 300000U};
    for (const uint32_t cap : kCaps) {
        const uint32_t minimum = cap / 2U;
        assert(backoff.NextDelayMs(0U) == minimum);
    }

    backoff.Reset();
    assert(backoff.NextDelayMs(UINT32_MAX) >= 5000U);
    assert(backoff.NextDelayMs(0U) == 10000U);
    return 0;
}
