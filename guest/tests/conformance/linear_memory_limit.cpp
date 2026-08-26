#include <cstdint>
#include <memory>
#include <new>

#include "sdk/micropixel.hpp"

int main() {
    constexpr size_t kBeyondHostLimitBytes = 9U * 1024U * 1024U;
    constexpr size_t kWithinHostLimitBytes = 7U * 1024U * 1024U;
    micropixel::Application app;
    const micropixel::Log log = app.log();
    log.Info("linear memory: test started");

    std::unique_ptr<uint8_t[]> rejected{new (std::nothrow) uint8_t[kBeyondHostLimitBytes]};
    micropixel::Assert(rejected == nullptr, "linear memory: Host limit was not enforced");
    log.Info("linear memory: 9 MiB rejected");

    std::unique_ptr<uint8_t[]> probe{new (std::nothrow) uint8_t[512U * 1024U]};
    micropixel::Assert(probe != nullptr, "linear memory: 512 KiB growth probe failed");
    log.Info("linear memory: 512 KiB allocated");
    probe.reset();
    log.Info("linear memory: 512 KiB released");

    std::unique_ptr<uint8_t[]> accepted{new (std::nothrow) uint8_t[kWithinHostLimitBytes]};
    micropixel::Assert(accepted != nullptr, "linear memory: in-policy allocation failed");
    log.Info("linear memory: 7 MiB allocated");
    accepted[0] = 0x5AU;
    accepted[kWithinHostLimitBytes - 1U] = 0xA5U;
    micropixel::Assert(accepted[0] == 0x5AU && accepted[kWithinHostLimitBytes - 1U] == 0xA5U,
                       "linear memory: grown allocation access failed");
    log.Info("linear memory: 7 MiB endpoints verified");
    accepted.reset();
    log.Info("linear memory: 7 MiB released");
    return 0;
}
