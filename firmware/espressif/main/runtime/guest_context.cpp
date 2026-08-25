#include "runtime/guest_context.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"

namespace micropixel::runtime {

namespace {
constexpr char kTag[] = "micropixel_guest";
}

GuestContext::GuestContext(const micropixel_aot_package_t& package, device::DeviceServices& devices,
                           GuestLogSink* log_sink)
    : devices_(devices),
      log_sink_(log_sink),
      clock_origin_us_(esp_timer_get_time()),
      events_{},
      timers_(events_, clock_origin_us_),
      resources_(package),
      storage_(package),
      touch_events_(events_, devices_.input(), clock_origin_us_),
      key_events_(events_, devices_.input(), clock_origin_us_),
      timer_endpoint_(*this),
      system_endpoint_{},
      storage_endpoint_(*this),
      resource_endpoint_(*this),
      random_endpoint_(*this),
      graphics_endpoint_(*this),
      input_endpoint_(*this),
      audio_endpoint_(*this),
      service_registry_(timer_endpoint_, system_endpoint_, storage_endpoint_, resource_endpoint_, random_endpoint_,
                        graphics_endpoint_, input_endpoint_, audio_endpoint_) {
    (void)std::snprintf(app_id_.data(), app_id_.size(), "%s", reinterpret_cast<const char*>(package.app_id));
}

GuestContext::~GuestContext() {
    key_events_.Shutdown();
    touch_events_.Shutdown();
    (void)devices_.audio().StopAll();
    (void)devices_.audio().ResumeAll();
    devices_.graphics().ReleaseGuestResources();
    resources_.Shutdown();
    events_.Close();
}

void GuestContext::WriteLog(uint32_t level, const uint8_t* bytes, uint32_t length) const {
    if (log_sink_ != nullptr) {
        log_sink_->WriteGuestLog(app_id_.data(), level, bytes, length, static_cast<uint64_t>(esp_timer_get_time()));
    }
}

bool GuestContext::Suspend(TickType_t timeout) {
    if (suspended_) {
        return true;
    }
    touch_events_.Suspend();
    key_events_.Suspend();
    const bool timers_suspended = timers_.Suspend();
    auto audio_result = devices_.audio().SuspendAll();
    const bool audio_suspended = audio_result || audio_result.error().status == MICROPIXEL_STATUS_UNSUPPORTED;
    if (!timers_suspended || !audio_suspended || !events_.Suspend(timeout)) {
        (void)timers_.Resume();
        (void)devices_.audio().ResumeAll();
        touch_events_.Resume();
        key_events_.Resume();
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
    key_events_.Resume();
    events_.Resume();
    suspended_ = false;
    ESP_LOGI(kTag, "Guest services resumed from the same AppSession: event=%" PRIu32, core_sequence_);
    return true;
}

bool GuestContext::RequestStop() {
    touch_events_.Suspend();
    key_events_.Suspend();
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
    key_events_.Suspend();
    (void)timers_.Suspend();
    (void)devices_.audio().SuspendAll();
    events_.Close();
}

bool GuestContext::ResolveTextureForGraphics(void* context, micropixel_texture_handle_t texture,
                                             device::BitmapView& view_out) {
    return context != nullptr && static_cast<GuestContext*>(context)->ResolveTexture(texture, view_out);
}

bool GuestContext::RetainTextureForGraphics(void* context, micropixel_texture_handle_t texture) {
    return context != nullptr && static_cast<GuestContext*>(context)->resources_.RetainSceneTexture(texture);
}

void GuestContext::ReleaseTextureForGraphics(void* context, micropixel_texture_handle_t texture) {
    if (context != nullptr) {
        static_cast<GuestContext*>(context)->resources_.ReleaseSceneTexture(texture);
    }
}

device::TextureAccess GuestContext::GraphicsTextureAccess() {
    return device::TextureAccess{
        .context = this,
        .resolve = ResolveTextureForGraphics,
        .retain = RetainTextureForGraphics,
        .release = ReleaseTextureForGraphics,
    };
}

device::DeviceResult<void> GuestContext::GraphicsSubmit(const uint8_t* bytes, uint32_t length) {
    uint64_t touch_sample_us = touch_events_.TakeSampleForGraphics();
    const device::TextureAccess textures = GraphicsTextureAccess();
    auto result = devices_.graphics().Submit(bytes, length, textures);
    if (result) {
        touch_events_.NoteGraphicsSubmitComplete(touch_sample_us);
    }
    return result;
}

device::DeviceResult<void> GuestContext::GraphicsBeginFrame() { return devices_.graphics().BeginFrame(); }

device::DeviceResult<void> GuestContext::GraphicsCommitFrame() {
    const device::TextureAccess textures = GraphicsTextureAccess();
    return devices_.graphics().CommitFrame(textures);
}

device::DeviceResult<void> GuestContext::GraphicsCancelFrame() { return devices_.graphics().CancelFrame(); }

ServiceResult<void> GuestContext::UpdateStreamingTexture(const micropixel_streaming_texture_update_request_t& update,
                                                         const uint8_t* pixels) {
    auto texture = resources_.MutableTexture(update.texture);
    if (!texture) {
        return FailService<void>(texture.error().status);
    }
    const uint32_t bytes_per_pixel = texture->pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888
                                         ? 3U
                                         : (texture->pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? 4U : 0U);
    if (pixels == nullptr || update.width == 0U || update.height == 0U || bytes_per_pixel == 0U ||
        static_cast<uint64_t>(update.x) + update.width > texture->width ||
        static_cast<uint64_t>(update.y) + update.height > texture->height ||
        update.pitch != update.width * bytes_per_pixel) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    auto result = devices_.graphics().UpdateBitmap(*texture, update.x, update.y, update.width, update.height, pixels,
                                                   update.pitch);
    return result ? ServiceResult<void>{} : FailService<void>(result.error().status);
}

ServiceResult<void> GuestContext::BeginTextureUpdateBatch() {
    auto result = devices_.graphics().BeginBitmapUpdateFrame();
    return result ? ServiceResult<void>{} : FailService<void>(result.error().status);
}

ServiceResult<void> GuestContext::FinishTextureUpdateBatch() {
    auto result = devices_.graphics().CommitBitmapUpdateFrame();
    return result ? ServiceResult<void>{} : FailService<void>(result.error().status);
}

}  // namespace micropixel::runtime
