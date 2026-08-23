#include "sdk/micropixel.hpp"

namespace {

bool DurationBetween(micropixel::Duration value, uint64_t minimum_us, uint64_t maximum_us) {
    uint64_t microseconds = value.count_microseconds();
    return microseconds >= minimum_us && microseconds <= maximum_us;
}

micropixel::TimerEvent WaitForTimer(const micropixel::Application& app, const micropixel::Timer& source) {
    micropixel::Event event = app.WaitEvent();
    const micropixel::TimerEvent* timer = event.TimerFrom(source);
    if (timer == nullptr) {
        __builtin_trap();
    }
    return *timer;
}

}  // namespace

int main() {
    micropixel::Application app;

    micropixel::Timer one_shot = app.timers().After(micropixel::Duration::Milliseconds(100));
    micropixel::TimePoint run_started = app.clock().Now();

    micropixel::TimerEvent one_shot_event = WaitForTimer(app, one_shot);
    if (!DurationBetween(one_shot_event.delta(), 80000U, 250000U) || one_shot_event.timestamp() < run_started) {
        return 34;
    }

    micropixel::Timer periodic = app.timers().Every(micropixel::Duration::Milliseconds(50));

    micropixel::TimePoint previous{};
    for (uint32_t count = 0; count < 4U; ++count) {
        micropixel::TimerEvent tick = WaitForTimer(app, periodic);
        if (!DurationBetween(tick.delta(), 30000U, 150000U) || (count > 0U && tick.timestamp() < previous)) {
            return 38;
        }
        previous = tick.timestamp();
    }
    periodic.Cancel();

    /* The guard Timer proves that no event from the cancelled Timer follows. */
    micropixel::Timer guard = app.timers().After(micropixel::Duration::Milliseconds(120));
    micropixel::TimerEvent guard_event = WaitForTimer(app, guard);
    if (!DurationBetween(guard_event.delta(), 100000U, 260000U)) {
        return 43;
    }

    /* Leave one fast periodic event pending while RAII churn keeps the Guest
       out of WaitEvent(). Further ticks must coalesce into that one event. */
    micropixel::Timer coalescing = app.timers().Every(micropixel::Duration::Milliseconds(1));
    (void)WaitForTimer(app, coalescing);

    /* Reuse Host slots repeatedly; move-only RAII must Release Every handle. */
    for (uint32_t iteration = 0; iteration < 256U; ++iteration) {
        micropixel::Timer temporary = app.timers().After(micropixel::Duration::Seconds(60));
        (void)temporary;
    }
    (void)WaitForTimer(app, coalescing);
    coalescing.Cancel();

    micropixel::Duration elapsed = app.clock().Now() - run_started;
    if (!DurationBetween(elapsed, 350000U, 900000U)) {
        return 46;
    }
    app.log().Info("timer_counter: one-shot, periodic, Cancel, Clock/TimePoint and RAII passed");
    return 0;
}
