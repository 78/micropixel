#include "runtime/service_binding.hpp"
#include "sdk/timer.hpp"

using micropixel::runtime::CallService;
using micropixel::runtime::CallVoid;
using micropixel::runtime::ErrorFromStatus;
using micropixel::runtime::OpenService;
using micropixel::runtime::ServiceCache;

namespace {

ServiceCache timer_service;

}  // namespace

namespace micropixel {

Timer::~Timer() { Reset(); }

Result<void> Timer::Cancel() {
    if (handle_ == 0U) return {};
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    int32_t status = OpenService(timer_service, MICROPIXEL_SERVICE_TIMER, 1U, 0U);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(timer_service, MICROPIXEL_TIMER_METHOD_DESTROY, &request, sizeof(request));
    }
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    handle_ = 0U;
    return {};
}

void Timer::Reset() {
    if (handle_ != 0U) {
        micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
        int32_t status = OpenService(timer_service, MICROPIXEL_SERVICE_TIMER, 1U, 0U);
        if (status == MICROPIXEL_STATUS_OK) {
            status = CallVoid(timer_service, MICROPIXEL_TIMER_METHOD_DESTROY, &request, sizeof(request));
        }
        handle_ = 0U;
        (void)status;
    }
}

Result<Timer> Timers::After(Duration delay) const { return Create(delay, false); }

Result<Timer> Timers::Every(Duration period) const { return Create(period, true); }

Result<Timer> Timers::Create(Duration delay, bool repeating) const {
    if (delay.count_microseconds() == 0U) return unexpected(Error{ErrorCode::kInvalidArgument});
    int32_t status = OpenService(timer_service, MICROPIXEL_SERVICE_TIMER, 1U, 0U);
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    micropixel_handle_response_t response{};
    uint32_t response_size = 0U;
    status = CallService(timer_service, MICROPIXEL_TIMER_METHOD_CREATE, nullptr, 0U, &response, sizeof(response),
                         response_size);
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.handle == 0U) {
        return unexpected(Error{ErrorCode::kInternal});
    }
    Timer timer{response.handle};
    micropixel_timer_start_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, response.handle,
                                             delay.count_microseconds(), repeating ? delay.count_microseconds() : 0U};
    status = CallVoid(timer_service, MICROPIXEL_TIMER_METHOD_START, &request, sizeof(request));
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    return timer;
}

}  // namespace micropixel
