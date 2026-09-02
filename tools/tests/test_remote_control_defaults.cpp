#include <cstdlib>
#include <iostream>

#include "host/controller/remote/remote_control_defaults.hpp"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    using micropixel::firmware::remote_control::EnabledByDefault;

    Check(!EnabledByDefault(""), "Remote Control should default to disabled without a service host");
    Check(EnabledByDefault("control.local"), "Remote Control should default to enabled for a DNS host");
    Check(EnabledByDefault("192.0.2.1"), "Remote Control should default to enabled for an IP host");

    std::cout << "remote control default policy tests passed\n";
    return 0;
}
