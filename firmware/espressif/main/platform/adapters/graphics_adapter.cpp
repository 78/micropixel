#include "platform/adapters/graphics_adapter.hpp"

namespace micropixel::platform::adapters {

bool GraphicsAdapter::Available() const { return operations_.available(operations_.context); }

int32_t GraphicsAdapter::GetInfo(micropixel_graphics_info_t& info) {
    return operations_.get_info(operations_.context, info);
}

int32_t GraphicsAdapter::Submit(const uint8_t* bytes, uint32_t length, const device::TextureAccess& textures) {
    return operations_.submit(operations_.context, bytes, length, textures);
}

int32_t GraphicsAdapter::LoadFont(const device::FontResourceView& resource, micropixel_font_info_t& info_out) {
    return operations_.load_font(operations_.context, resource, info_out);
}

int32_t GraphicsAdapter::ReleaseFont(micropixel_font_handle_t font) {
    return operations_.release_font(operations_.context, font);
}

int32_t GraphicsAdapter::MeasureText(micropixel_font_handle_t font, const char* text, uint32_t text_length,
                                     micropixel_text_metrics_t& metrics_out) {
    return operations_.measure_text(operations_.context, font, text, text_length, metrics_out);
}

int32_t GraphicsAdapter::BeginBitmapUpdateFrame() { return operations_.begin_bitmap_update_frame(operations_.context); }

int32_t GraphicsAdapter::UpdateBitmap(const device::BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width,
                                      uint32_t height, const uint8_t* pixels, uint32_t stride) {
    return operations_.update_bitmap(operations_.context, bitmap, x, y, width, height, pixels, stride);
}

int32_t GraphicsAdapter::CommitBitmapUpdateFrame() {
    return operations_.commit_bitmap_update_frame(operations_.context);
}

int32_t GraphicsAdapter::ScaleBitmap(const device::BitmapView& source, const device::BitmapView& destination) {
    return operations_.scale_bitmap(operations_.context, source, destination);
}

int32_t GraphicsAdapter::ShowLaunchBitmap(const device::BitmapView& bitmap) {
    return operations_.show_launch_bitmap(operations_.context, bitmap);
}

void GraphicsAdapter::DismissLaunchBitmap() { operations_.dismiss_launch_bitmap(operations_.context); }

void GraphicsAdapter::ReleaseGuestResources() { operations_.release_guest_resources(operations_.context); }

int32_t GraphicsAdapter::CreateDirectSurface(const device::DirectSurfaceConfig& config,
                                             const device::DirectSurfaceReleaseSink& sink,
                                             device::DirectSurfaceInfo& info_out) {
    if (operations_.create_direct_surface == nullptr) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    return operations_.create_direct_surface(operations_.context, config, sink, info_out);
}

int32_t GraphicsAdapter::PresentDirectSurface(const device::DirectSurfacePresentation& presentation) {
    if (operations_.present_direct_surface == nullptr) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    return operations_.present_direct_surface(operations_.context, presentation);
}

void GraphicsAdapter::SuspendDirectSurface() {
    if (operations_.suspend_direct_surface != nullptr) {
        operations_.suspend_direct_surface(operations_.context);
    }
}

void GraphicsAdapter::ResumeDirectSurface() {
    if (operations_.resume_direct_surface != nullptr) {
        operations_.resume_direct_surface(operations_.context);
    }
}

int32_t GraphicsAdapter::DestroyDirectSurface() {
    if (operations_.destroy_direct_surface == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    return operations_.destroy_direct_surface(operations_.context);
}

}  // namespace micropixel::platform::adapters
