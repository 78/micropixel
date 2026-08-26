#include "sdk/micropixel.hpp"

using micropixel::literals::operator""_ms;

extern "C" __attribute__((export_name("__micropixel_test_run_handler"))) void __micropixel_test_run_handler(void) {}

int main() {
    micropixel::Application app;
    micropixel::Timer once = app.timers().After(20_ms);
    bool fired = false;
    app.Run([&](const micropixel::Event& event) -> micropixel::EventResult {
        if (event.TimerFrom(once) != nullptr) {
            fired = true;
            once.Reset();
            return micropixel::EventResult::kExit;
        }
        return micropixel::EventResult::kContinue;
    });
    if (!fired) {
        return 61;
    }
    app.log().Info("run_handler_after: EventResult::kExit returned from Run before Host Stop");
    return 0;
}
