#include "sdk/micropixel.hpp"

using micropixel::literals::operator""_ms;

int main() {
    micropixel::Application app;
    auto repeating = app.Every(10_ms, [](const micropixel::TimerEvent& tick) { (void)tick.delta(); });
    auto once = app.After(20_ms, [] {});
    app.Run(repeating, once);
}
