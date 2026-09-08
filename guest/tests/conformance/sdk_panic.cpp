#include "sdk/micropixel.hpp"

using micropixel::literals::operator""_ms;

int main() {
    micropixel::Application app;
    (void)app.timers().Every(0_ms).value();
    return 99;
}
