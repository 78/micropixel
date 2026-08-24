#include "sdk/micropixel.hpp"

using micropixel::literals::operator""_ms;

extern "C" __attribute__((export_name("__micropixel_test_run_handler"))) void __micropixel_test_run_handler(void) {}

int main() {
    micropixel::Application app;
    micropixel::Timer once = app.timers().After(20_ms);
    bool fired = false;
    bool saw_stop = false;
    app.Run([&](const micropixel::Event& event) {
        if (event.TimerFrom(once) != nullptr) {
            fired = true;
            once.Reset();
        }
        if (event.type() == micropixel::EventType::kStop) {
            saw_stop = true;
        }
    });
    if (!fired || !saw_stop) {
        return 61;
    }
    app.log().Info("run_handler_after: Timer fired; Stop delivered; Run returned");
    return 0;
}
