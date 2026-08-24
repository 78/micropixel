#include "sdk/micropixel.hpp"

using micropixel::literals::operator""_ms;

extern "C" __attribute__((export_name("__micropixel_test_run_handler"))) void __micropixel_test_run_handler(void) {}

int main() {
    micropixel::Application app;
    micropixel::Timer repeating = app.timers().Every(10_ms);
    micropixel::Timer once = app.timers().After(20_ms);
    uint32_t repeating_count = 0U;
    bool once_fired = false;
    bool saw_stop = false;
    app.Run([&](const micropixel::Event& event) {
        if (const micropixel::TimerEvent* tick = event.TimerFrom(repeating)) {
            (void)tick->delta();
            ++repeating_count;
        }
        if (event.TimerFrom(once) != nullptr) {
            once_fired = true;
            once.Reset();
        }
        if (event.type() == micropixel::EventType::kStop) {
            saw_stop = true;
        }
    });
    if (repeating_count == 0U || !once_fired || !saw_stop) {
        return 62;
    }
    app.log().Info("run_handler_multiple: multiple Timers dispatched; Stop returned from Run");
    return 0;
}
