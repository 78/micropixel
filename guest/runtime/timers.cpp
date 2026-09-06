#include "runtime/service_binding.hpp"
#include "sdk/timer.hpp"

using micropixel::runtime::CallService;
using micropixel::runtime::CallVoid;
using micropixel::runtime::OpenService;
using micropixel::runtime::RequireOk;
using micropixel::runtime::ServiceCache;

namespace {

ServiceCache timer_service;

}  // namespace

namespace micropixel {

Timer::~Timer() { Reset(); }

void Timer::Cancel() {
    if (handle_ == 0U) {
        return;
    }
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    RequireOk(OpenService(timer_service, MICROPIXEL_SERVICE_TIMER, 1U, 0U), "timer.cancel.open");
    RequireOk(CallVoid(timer_service, MICROPIXEL_TIMER_METHOD_RELEASE, &request, sizeof(request)), "timer.cancel");
    handle_ = 0U;
}

void Timer::Reset() {
    if (handle_ != 0U) {
        micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
        int32_t status = OpenService(timer_service, MICROPIXEL_SERVICE_TIMER, 1U, 0U);
        if (status == MICROPIXEL_STATUS_OK) {
            status = CallVoid(timer_service, MICROPIXEL_TIMER_METHOD_RELEASE, &request, sizeof(request));
        }
        handle_ = 0U;
        (void)status;
    }
}

Timer Timers::After(Duration delay) const {
    RequireOk(OpenService(timer_service, MICROPIXEL_SERVICE_TIMER, 1U, 0U), "timers.after.open");
    micropixel_handle_response_t response{};
    uint32_t response_size = 0U;
    RequireOk(CallService(timer_service, MICROPIXEL_TIMER_METHOD_CREATE, nullptr, 0U, &response, sizeof(response),
                          response_size),
              "timers.after.create");
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.handle == 0U) {
        runtime::Panic("timers.after.response", MICROPIXEL_STATUS_INTERNAL);
    }
    Timer timer{response.handle};
    micropixel_timer_start_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, response.handle,
                                             delay.count_microseconds(), 0U};
    RequireOk(CallVoid(timer_service, MICROPIXEL_TIMER_METHOD_START, &request, sizeof(request)), "timers.after.start");
    return timer;
}

Timer Timers::Every(Duration period) const {
    RequireOk(OpenService(timer_service, MICROPIXEL_SERVICE_TIMER, 1U, 0U), "timers.every.open");
    micropixel_handle_response_t response{};
    uint32_t response_size = 0U;
    RequireOk(CallService(timer_service, MICROPIXEL_TIMER_METHOD_CREATE, nullptr, 0U, &response, sizeof(response),
                          response_size),
              "timers.every.create");
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.handle == 0U) {
        runtime::Panic("timers.every.response", MICROPIXEL_STATUS_INTERNAL);
    }
    Timer timer{response.handle};
    micropixel_timer_start_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, response.handle,
                                             period.count_microseconds(), period.count_microseconds()};
    RequireOk(CallVoid(timer_service, MICROPIXEL_TIMER_METHOD_START, &request, sizeof(request)), "timers.every.start");
    return timer;
}

}  // namespace micropixel
