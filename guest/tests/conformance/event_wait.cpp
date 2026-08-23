#include "abi/micropixel_abi.h"
#include "sdk/micropixel.hpp"

extern "C" __attribute__((export_name("__micropixel_test_event_wait"))) int32_t __micropixel_test_event_wait() {
    return 1;
}

int main() {
    micropixel_event_t invalid{};
    int32_t invalid_status = micropixel_event_wait(&invalid, sizeof(invalid) - 1U, 0U);
    if (invalid_status != MICROPIXEL_STATUS_BUFFER_TOO_SMALL) {
        return 20;
    }
    micropixel_event_t empty{};
    if (micropixel_event_wait(&empty, sizeof(empty), 0U) != MICROPIXEL_STATUS_TIMEOUT) {
        return 21;
    }

    micropixel::Application app;
    app.log().Info("event_wait: invalid buffer rejected; non-blocking timeout passed");

    micropixel::TimePoint previous{};
    for (uint32_t count = 0; count < 3U; ++count) {
        micropixel::Event event = app.WaitEvent();
        if (event.type() != micropixel::EventType::kUnknown) {
            return 24;
        }
        if (count > 0U && event.timestamp() < previous) {
            return 25;
        }
        previous = event.timestamp();
    }

    app.log().Info("event_wait: 3 ordered events; blocking wait complete");
    return 0;
}
