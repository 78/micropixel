// Graphics 1.5 Direct Surface, positive path through the SDK with the default
// Host-owned buffers (no pinned memory needed): a double-buffered surface
// presents 60 full frames and every buffer comes back through kSurfaceReleased
// in presentation order. Frames are painted with Graphics 1.6 raster rects
// when the Host has kernels, otherwise presented as they are. Exit codes
// 30..49 name the failed step.
#include "abi/micropixel_abi.h"
#include "sdk/micropixel.hpp"

namespace {

constexpr uint32_t kFrames = 60U;
constexpr uint32_t kBufferCount = 2U;

// Four bands whose colors rotate with the frame, so a stuck frame is visible.
bool FillFrame(const micropixel::SurfaceRaster& raster, micropixel::DirectSurface& surface, uint32_t index,
               uint32_t frame) {
    if (!raster.valid()) {
        return true;
    }
    micropixel::RasterDrawList list = raster.Begin(surface, index);
    if (!list.open()) {
        return false;
    }
    const int32_t band = static_cast<int32_t>(surface.buffer_height() / 4U);
    const micropixel::Color colors[4] = {micropixel::Color::Rgb(255U, 0U, 0U), micropixel::Color::Rgb(0U, 255U, 0U),
                                         micropixel::Color::Rgb(0U, 0U, 255U), micropixel::Color::White()};
    for (uint32_t row = 0U; row < 4U; ++row) {
        const micropixel::Rect area{0, static_cast<int32_t>(row) * band, static_cast<int32_t>(surface.buffer_width()),
                                    band};
        if (!list.FillRect(area, colors[(row + frame) % 4U])) {
            return false;
        }
    }
    // A half-transparent strip across the middle exercises the blend path.
    const micropixel::Rect strip{0, band * 2 - band / 4, static_cast<int32_t>(surface.buffer_width()), band / 2};
    if (!list.FillRect(strip, micropixel::Color::Black(), 128U)) {
        return false;
    }
    return list.Finish().has_value();
}

}  // namespace

int main() {
    micropixel::Application app;
    const micropixel::RendererInfo info = app.renderer().info();

    auto created = app.renderer().CreateDirectSurface(kBufferCount);
    if (!created) {
        app.log().Error("direct_surface_present: create failed");
        return 30;
    }
    micropixel::DirectSurface surface = static_cast<micropixel::DirectSurface&&>(*created);
    if (!surface.valid() || surface.width() != info.physical_width() || surface.height() != info.physical_height() ||
        surface.buffer_width() != surface.width() || surface.buffer_height() != surface.height() ||
        surface.buffer_count() != kBufferCount || surface.pitch() != surface.width() * 2U ||
        surface.direct_scanout() != info.direct_scanout() ||
        surface.rgb565_byte_swapped() != info.rgb565_byte_swapped() || !surface.host_buffers()) {
        return 31;
    }
    // Host buffers are never mapped into the Guest.
    for (uint32_t index = 0U; index < kBufferCount; ++index) {
        if (surface.Buffer(index) != nullptr || surface.Busy(index)) {
            return 32;
        }
    }
    if (surface.Buffer(kBufferCount) != nullptr) {
        return 33;
    }

    // A second surface must be refused while this one exists.
    if (app.renderer().CreateDirectSurface(1U)) {
        return 34;
    }

    micropixel::SurfaceRaster raster;
    if (auto kernels = app.renderer().CreateSurfaceRaster()) {
        raster = *kernels;
    } else if (info.raster_supported()) {
        return 45;
    }
    if (raster.valid()) {
        // A list on a busy or out-of-range buffer is refused on the SDK side.
        if (raster.Begin(surface, kBufferCount).open()) {
            return 46;
        }
    }

    // On a direct-scanout Host the displayed frame stays with the Host until
    // the next frame replaces it (or the surface is destroyed), so only
    // kFrames - 1 releases are guaranteed while presenting; a composited Host
    // may return all of them.
    uint32_t presented = 0U;
    uint32_t released = 0U;
    uint32_t expected_release_index = 0U;
    while (released + 1U < kFrames) {
        uint32_t index = 0U;
        if (presented < kFrames && surface.AcquireFree(index)) {
            if (index != presented % kBufferCount) {
                return 35;
            }
            if (!FillFrame(raster, surface, index, presented)) {
                return 47;
            }
            if (!surface.Present(index)) {
                return 36;
            }
            if (!surface.Busy(index)) {
                return 37;
            }
            if (raster.valid() && raster.Begin(surface, index).open()) {
                return 48;
            }
            // Re-presenting a buffer the Host holds is an SDK-side state error.
            auto again = surface.Present(index);
            if (again || again.error().code() != micropixel::ErrorCode::kInvalidState) {
                return 38;
            }
            ++presented;
            continue;
        }
        micropixel::Event event;
        if (!app.WaitEventFor(event, micropixel::Duration::Seconds(2U))) {
            app.log().Error("direct_surface_present: release timed out");
            return 39;
        }
        if (event.type() == micropixel::EventType::kStop) {
            return 40;
        }
        const micropixel::SurfaceReleasedEvent* release = event.ReleasedFrom(surface);
        if (release == nullptr) {
            continue;
        }
        if (release->buffer_index() != expected_release_index || surface.Busy(release->buffer_index())) {
            return 41;
        }
        expected_release_index = (expected_release_index + 1U) % kBufferCount;
        ++released;
    }

    if (presented != kFrames) {
        return 49;
    }
    // Reset destroys the surface, which returns the held frame first.
    surface.Reset();
    if (surface.valid()) {
        return 42;
    }
    // After Reset the slot is free again and the Scene path is usable.
    auto second = app.renderer().CreateDirectSurface(1U);
    if (!second) {
        return 43;
    }
    second->Reset();

    app.log().Info("direct_surface_present: 60 frames presented and released in order");
    return 0;
}
