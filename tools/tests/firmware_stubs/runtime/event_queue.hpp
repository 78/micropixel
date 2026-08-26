#ifndef MICROPIXEL_TEST_STUB_RUNTIME_EVENT_QUEUE_HPP
#define MICROPIXEL_TEST_STUB_RUNTIME_EVENT_QUEUE_HPP

#include "abi/micropixel_abi.h"

namespace micropixel::runtime {

enum class GpioPushResult {
    kEnqueued,
    kCoalesced,
    kFailed,
};

class EventQueue final {
   public:
    [[nodiscard]] GpioPushResult PushGpioCoalesced(const micropixel_event_t&) {
        return GpioPushResult::kEnqueued;
    }
};

}  // namespace micropixel::runtime

#endif
