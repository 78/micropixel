#include "runtime/guest_context.hpp"

#include <cinttypes>
#include <cstdint>

#include "esp_log.h"
#include "esp_timer.h"

namespace micropixel::runtime {

namespace {
constexpr char kTag[] = "micropixel_guest";
}

GuestContext::GuestContext(const micropixel_aot_package_t& package, device::DeviceServices& devices)
    : devices_(devices),
      clock_origin_us_(esp_timer_get_time()),
      events_{},
      timers_(events_, clock_origin_us_),
      resources_(package, events_, clock_origin_us_),
      storage_(package),
      touch_events_(events_, devices_.input(), clock_origin_us_),
      timer_endpoint_(*this),
      storage_endpoint_(*this),
      resource_endpoint_(*this),
      random_endpoint_(*this),
      graphics_endpoint_(*this),
      input_endpoint_(*this),
      audio_endpoint_(*this),
      service_registry_(timer_endpoint_, storage_endpoint_, resource_endpoint_, random_endpoint_, graphics_endpoint_,
                        input_endpoint_, audio_endpoint_) {}

GuestContext::~GuestContext() {
    touch_events_.Shutdown();
    (void)devices_.audio().StopAll();
    (void)devices_.audio().ResumeAll();
    devices_.graphics().ReleaseGuestResources();
    resources_.Shutdown();
    events_.Close();
}

bool GuestContext::Suspend(TickType_t timeout) {
    if (suspended_) {
        return true;
    }
    touch_events_.Suspend();
    const bool timers_suspended = timers_.Suspend();
    auto audio_result = devices_.audio().SuspendAll();
    const bool audio_suspended = audio_result || audio_result.error().status == MICROPIXEL_STATUS_UNSUPPORTED;
    if (!timers_suspended || !audio_suspended || !events_.Suspend(timeout)) {
        (void)timers_.Resume();
        (void)devices_.audio().ResumeAll();
        touch_events_.Resume();
        events_.Resume();
        return false;
    }
    suspended_ = true;
    ESP_LOGI(kTag, "Guest services suspended at a WaitEvent safe point");
    return true;
}

bool GuestContext::Resume() {
    if (!suspended_) {
        return true;
    }
    micropixel_event_t resume_event{};
    resume_event.size = sizeof(resume_event);
    resume_event.event_id = MICROPIXEL_CORE_EVENT_RESUME;
    resume_event.timestamp_us = timers_.Now();
    resume_event.sequence = ++core_sequence_;
    resume_event.status = MICROPIXEL_STATUS_OK;
    if (!events_.PrepareResume(resume_event)) {
        return false;
    }
    const bool timers_resumed = timers_.Resume();
    auto audio_result = devices_.audio().ResumeAll();
    const bool audio_resumed = audio_result || audio_result.error().status == MICROPIXEL_STATUS_UNSUPPORTED;
    if (!timers_resumed || !audio_resumed) {
        // Wake the safe point even on failure. AppController immediately
        // requests stop, and a blocked Guest must be allowed to observe it.
        events_.Resume();
        return false;
    }
    touch_events_.Resume();
    events_.Resume();
    suspended_ = false;
    ESP_LOGI(kTag, "Guest services resumed from the same AppSession: event=%" PRIu32, core_sequence_);
    return true;
}

bool GuestContext::RequestStop() {
    touch_events_.Suspend();
    (void)timers_.Suspend();
    (void)devices_.audio().SuspendAll();
    micropixel_event_t stop_event{};
    stop_event.size = sizeof(stop_event);
    stop_event.event_id = MICROPIXEL_CORE_EVENT_STOP;
    stop_event.timestamp_us = timers_.Now();
    stop_event.sequence = ++core_sequence_;
    stop_event.status = MICROPIXEL_STATUS_OK;
    const bool requested = events_.RequestStop(stop_event);
    if (requested) {
        ESP_LOGI(kTag, "Guest cooperative stop requested: event=%" PRIu32, core_sequence_);
    }
    return requested;
}

void GuestContext::ForceStop() {
    touch_events_.Suspend();
    (void)timers_.Suspend();
    (void)devices_.audio().SuspendAll();
    events_.Close();
}

bool GuestContext::ResolveBitmapForGraphics(void* context, micropixel_bitmap_handle_t bitmap,
                                            device::BitmapView& view_out) {
    return context != nullptr && static_cast<GuestContext*>(context)->ResolveBitmap(bitmap, view_out);
}

device::DeviceResult<void> GuestContext::GraphicsSubmit(const uint8_t* bytes, uint32_t length) {
    uint64_t touch_sample_us = touch_events_.TakeSampleForGraphics();
    auto result = devices_.graphics().Submit(bytes, length, ResolveBitmapForGraphics, this);
    if (result) {
        touch_events_.NoteGraphicsSubmitComplete(touch_sample_us);
    }
    return result;
}

device::DeviceResult<void> GuestContext::GraphicsBeginFrame() { return devices_.graphics().BeginFrame(); }

device::DeviceResult<void> GuestContext::GraphicsCommitFrame() {
    return devices_.graphics().CommitFrame(ResolveBitmapForGraphics, this);
}

ServiceResult<void> GuestContext::UpdateOffscreenSurface(const micropixel_offscreen_surface_update_request_t& update,
                                                         const uint8_t* pixels) {
    auto bitmap = resources_.MutableBitmap(update.bitmap);
    if (!bitmap) {
        return FailService<void>(bitmap.error().status);
    }
    const uint32_t bytes_per_pixel = bitmap->pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB888
                                         ? 3U
                                         : (bitmap->pixel_format == MICROPIXEL_PIXEL_FORMAT_ARGB8888 ? 4U : 0U);
    if (pixels == nullptr || update.width == 0U || update.height == 0U || bytes_per_pixel == 0U ||
        static_cast<uint64_t>(update.x) + update.width > bitmap->width ||
        static_cast<uint64_t>(update.y) + update.height > bitmap->height ||
        update.stride != update.width * bytes_per_pixel) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    auto result = devices_.graphics().UpdateBitmap(*bitmap, update.x, update.y, update.width, update.height, pixels,
                                                   update.stride);
    return result ? ServiceResult<void>{} : FailService<void>(result.error().status);
}

ServiceResult<void> GuestContext::BeginOffscreenUpdateFrame() {
    auto result = devices_.graphics().BeginBitmapUpdateFrame();
    return result ? ServiceResult<void>{} : FailService<void>(result.error().status);
}

ServiceResult<void> GuestContext::CommitOffscreenUpdateFrame() {
    auto result = devices_.graphics().CommitBitmapUpdateFrame();
    return result ? ServiceResult<void>{} : FailService<void>(result.error().status);
}

}  // namespace micropixel::runtime
