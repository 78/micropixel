#include <cassert>
#include <cstring>

#include "runtime/service_binding.hpp"
#include "sdk/application.hpp"

namespace {
int32_t open_status{}, create_status{}, start_status{}, release_status{};
uint32_t creates{}, starts{}, releases{}, next_handle{1}, last_handle{};
bool malformed{};
uint64_t initial_delay{}, period{};
}  // namespace
namespace micropixel {
Application::Application() noexcept = default;
}  // namespace micropixel
namespace micropixel::runtime {
int32_t OpenService(ServiceCache&, uint32_t id, uint16_t major, uint16_t minor) {
    assert(id == MICROPIXEL_SERVICE_TIMER && major == 1 && minor == 0);
    return open_status;
}
int32_t CallService(ServiceCache&, uint32_t method, const void*, uint32_t, void* response, uint32_t capacity,
                    uint32_t& response_size) {
    assert(method == MICROPIXEL_TIMER_METHOD_CREATE && capacity >= sizeof(micropixel_handle_response_t));
    ++creates;
    if (create_status != 0) return create_status;
    micropixel_handle_response_t value{};
    value.size = sizeof(value);
    value.handle = malformed ? 0 : next_handle++;
    last_handle = value.handle;
    std::memcpy(response, &value, sizeof(value));
    response_size = sizeof(value);
    return 0;
}
int32_t CallVoid(ServiceCache&, uint32_t method, const void* request, uint32_t size) {
    if (method == MICROPIXEL_TIMER_METHOD_START) {
        assert(size == sizeof(micropixel_timer_start_request_t));
        const auto& value = *static_cast<const micropixel_timer_start_request_t*>(request);
        initial_delay = value.initial_delay_us;
        period = value.period_us;
        ++starts;
        return start_status;
    }
    assert(method == MICROPIXEL_TIMER_METHOD_DESTROY && size == sizeof(micropixel_handle_request_t));
    assert(static_cast<const micropixel_handle_request_t*>(request)->handle == last_handle);
    ++releases;
    return release_status;
}
}  // namespace micropixel::runtime
int main() {
    using namespace micropixel;
    Application app;
    const auto timers = app.timers();
    assert(timers.Every(Duration{}).error().code() == ErrorCode::kInvalidArgument);
    assert(timers.After(Duration{}).error().code() == ErrorCode::kInvalidArgument);
    assert(creates == 0);
    open_status = MICROPIXEL_STATUS_UNSUPPORTED;
    assert(timers.After(Duration::Seconds(1)).error().code() == ErrorCode::kUnsupported);
    assert(creates == 0);
    open_status = 0;
    create_status = MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    assert(timers.After(Duration::Seconds(1)).error().code() == ErrorCode::kResourceExhausted);
    assert(starts == 0 && releases == 0);
    create_status = 0;
    malformed = true;
    assert(timers.After(Duration::Seconds(1)).error().code() == ErrorCode::kInternal);
    assert(starts == 0);
    malformed = false;
    start_status = MICROPIXEL_STATUS_INTERNAL;
    assert(!timers.After(Duration::Seconds(1)));
    assert(releases == 1);  // Failed start releases the newly acquired resource.
    start_status = 0;
    {
        auto timer = timers.Every(Duration::Milliseconds(20)).value();
        assert(initial_delay == 20000 && period == initial_delay);
        release_status = MICROPIXEL_STATUS_WOULD_BLOCK;
        assert(timer.Cancel().error().code() == ErrorCode::kWouldBlock && timer.valid());
        release_status = 0;
        assert(timer.Cancel() && !timer.valid());
        const auto count = releases;
        assert(timer.Cancel() && releases == count);
        auto once = timers.After(Duration::Seconds(1)).value();
        assert(period == 0 && initial_delay == 1000000);
        auto moved = static_cast<Timer&&>(once);
        assert(!once.valid() && moved.valid());
        release_status = MICROPIXEL_STATUS_INTERNAL;
        moved.Reset();
        assert(!moved.valid());
    }
    assert(releases == 4);
}
