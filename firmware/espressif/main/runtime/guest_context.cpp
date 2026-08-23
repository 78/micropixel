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
    devices_.graphics().ReleaseGuestResources();
    resources_.Shutdown();
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

device::DeviceResult<void> GuestContext::GraphicsBeginFrame() {
    return devices_.graphics().BeginFrame();
}

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
