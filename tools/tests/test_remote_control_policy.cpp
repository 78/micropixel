#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "host/controller/remote/remote_control_defaults.hpp"
#include "host/controller/remote/remote_reconnect_policy.hpp"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void TestRemoteControlDefaults() {
    using micropixel::firmware::remote_control::EnabledByDefault;

    Check(!EnabledByDefault(""), "Remote Control should default to disabled without a service host");
    Check(EnabledByDefault("control.local"), "Remote Control should default to enabled for a DNS host");
    Check(EnabledByDefault("192.0.2.1"), "Remote Control should default to enabled for an IP host");
}

using micropixel::firmware::remote_control::ReconnectBackoff;
using micropixel::firmware::remote_control::ShouldResetIdentityForControlStatus;

void TestRemoteControlReconnectPolicy() {
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
}

}  // namespace

int main() {
    TestRemoteControlDefaults();
    TestRemoteControlReconnectPolicy();
    return 0;
}
