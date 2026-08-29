#include <cassert>
#include <cstring>
#include <ctime>

#include "host/time/system_time.hpp"

using micropixel::firmware::system_time::FormatBeijingClock;
using micropixel::firmware::system_time::IsTrustedUtcTime;

int main() {
    assert(!IsTrustedUtcTime(static_cast<std::time_t>(1704067199)));
    assert(IsTrustedUtcTime(static_cast<std::time_t>(1704067200)));
    assert(IsTrustedUtcTime(static_cast<std::time_t>(4102444799)));
    assert(!IsTrustedUtcTime(static_cast<std::time_t>(4102444800)));

    assert(std::strcmp(FormatBeijingClock(0).data(), "--:--") == 0);
    assert(std::strcmp(FormatBeijingClock(static_cast<std::time_t>(1704067200)).data(), "08:00") == 0);
    assert(std::strcmp(FormatBeijingClock(static_cast<std::time_t>(1704124799)).data(), "23:59") == 0);
    assert(std::strcmp(FormatBeijingClock(static_cast<std::time_t>(1704124800)).data(), "00:00") == 0);
    return 0;
}
