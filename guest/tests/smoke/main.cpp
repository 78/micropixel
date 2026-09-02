#include "sdk/micropixel.hpp"

int main() {
    micropixel::Application app;
    micropixel::Log log = app.log();
    log.Info("Runtime Guest started: ABI, startup and log import passed");

    const micropixel::TimePoint started = app.clock().Now();
    micropixel::Timer timer = app.timers().After(micropixel::Duration::Milliseconds(100U));
    micropixel::Event event;
    if (!app.WaitEventFor(event, micropixel::Duration::Seconds(2U))) {
        log.Error("Runtime Guest failed: event_wait timed out");
        return 20;
    }
    const micropixel::TimerEvent* timer_event = event.TimerFrom(timer);
    if (timer_event == nullptr || timer_event->timestamp() < started) {
        log.Error("Runtime Guest failed: Timer event was invalid");
        return 21;
    }
    const uint64_t elapsed_us = (app.clock().Now() - started).count_microseconds();
    if (elapsed_us < 50000U || elapsed_us > 2000000U) {
        log.Error("Runtime Guest failed: clock delta was outside the smoke-test window");
        return 22;
    }

    log.Info("PASS: AOT ABI, log, clock, event_wait and Timer service passed");
    return 0;
}
