#include "platform/lvgl/guest_graphics_operations.hpp"

#include <cstddef>

namespace micropixel::platform::lvgl {
namespace {

GuestGraphicsOperationsContext& Binding(void* context) {
    return *static_cast<GuestGraphicsOperationsContext*>(context);
}

#if !CONFIG_MICROPIXEL_MOSAICO_SOFTWARE_RENDERING
constexpr uint32_t kPpaScaleFractionSteps = 16U;

bool ExactPpaScale(uint32_t source, uint32_t destination, float& scale_out) {
    const uint64_t quantized = (static_cast<uint64_t>(destination) * kPpaScaleFractionSteps + source - 1U) / source;
    if (quantized == 0U || quantized >= 256U * kPpaScaleFractionSteps ||
        static_cast<uint64_t>(source) * quantized / kPpaScaleFractionSteps != destination) {
        return false;
    }
    scale_out = static_cast<float>(quantized) / static_cast<float>(kPpaScaleFractionSteps);
    return true;
}
#endif

void ScaleBitmapNearest(const device::BitmapView& source, const device::BitmapView& destination,
                        uint32_t bytes_per_pixel) {
    auto* output = const_cast<uint8_t*>(destination.data);
    for (uint32_t destination_y = 0U; destination_y < destination.height; ++destination_y) {
        const uint32_t source_y =
            static_cast<uint32_t>(((static_cast<uint64_t>(destination_y) * 2U + 1U) * source.height) /
                                  (static_cast<uint64_t>(destination.height) * 2U));
        const uint8_t* source_row = source.data + static_cast<size_t>(source_y) * source.stride;
        uint8_t* destination_row = output + static_cast<size_t>(destination_y) * destination.stride;
        for (uint32_t destination_x = 0U; destination_x < destination.width; ++destination_x) {
            const uint32_t source_x =
                static_cast<uint32_t>(((static_cast<uint64_t>(destination_x) * 2U + 1U) * source.width) /
                                      (static_cast<uint64_t>(destination.width) * 2U));
            const uint8_t* source_pixel = source_row + static_cast<size_t>(source_x) * bytes_per_pixel;
            uint8_t* destination_pixel = destination_row + static_cast<size_t>(destination_x) * bytes_per_pixel;
            for (uint32_t channel = 0U; channel < bytes_per_pixel; ++channel) {
                destination_pixel[channel] = source_pixel[channel];
            }
        }
    }
}

int32_t ScaleBitmap(GuestGraphicsOperationsContext& binding, const device::BitmapView& source,
                    const device::BitmapView& destination) {
    if (source.data == nullptr || destination.data == nullptr || source.width == 0U || source.height == 0U ||
        destination.width == 0U || destination.height == 0U || source.pixel_format != destination.pixel_format ||
        source.flags != 0U || destination.flags != 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    uint32_t bytes_per_pixel = 0U;
    if (source.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888) {
        bytes_per_pixel = 3U;
    } else if (source.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888) {
        bytes_per_pixel = 4U;
    } else {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (source.stride != source.width * bytes_per_pixel || destination.stride < destination.width * bytes_per_pixel ||
        destination.stride % bytes_per_pixel != 0U || source.size < source.stride * source.height ||
        destination.size < destination.stride * destination.height) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
#if CONFIG_MICROPIXEL_MOSAICO_SOFTWARE_RENDERING
    if (!binding.engine->ScaleBitmapSoftware(source, destination)) {
        ScaleBitmapNearest(source, destination, bytes_per_pixel);
    }
    return MICROPIXEL_STATUS_OK;
#else
    const ppa_srm_color_mode_t mode =
        source.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888 ? PPA_SRM_COLOR_MODE_RGB888 : PPA_SRM_COLOR_MODE_ARGB8888;
    float scale_x = 0.0F;
    float scale_y = 0.0F;
    if (!ExactPpaScale(source.width, destination.width, scale_x) ||
        !ExactPpaScale(source.height, destination.height, scale_y)) {
        // PPA SRM exposes only four fractional scale bits. For ratios such as
        // 720 -> 480, asking it for 2/3 actually produces 10/16, leaving an
        // uninitialized macro-block band. Resource loading is off the frame
        // path, so use one exact CPU pass when the hardware ratio cannot
        // represent the requested dimensions; drawing remains a 1:1 blit.
        if (!binding.engine->ScaleBitmapSoftware(source, destination)) {
            ScaleBitmapNearest(source, destination, bytes_per_pixel);
        }
        return MICROPIXEL_STATUS_OK;
    }
    if (!binding.texture_scaler.Ready() && binding.texture_scaler.Initialize() != ESP_OK) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    const esp_err_t status = binding.texture_scaler.Blit({
        .source = source.data,
        .source_width = source.width,
        .source_height = source.height,
        .source_region = {.x = 0U, .y = 0U, .width = source.width, .height = source.height},
        .source_mode = mode,
        .destination = const_cast<uint8_t*>(destination.data),
        // PPA/DMA2D processes rows in hardware blocks. Keep the visible
        // bitmap width exact while using its padded stride as the physical
        // output pitch so the last block cannot wrap into the next row.
        .destination_width = destination.stride / bytes_per_pixel,
        .destination_height = destination.height,
        .destination_allocation_bytes = destination.size,
        .destination_x = 0U,
        .destination_y = 0U,
        .destination_mode = mode,
        .scale_x = scale_x,
        .scale_y = scale_y,
    });
    return status == ESP_OK ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
#endif
}

}  // namespace

adapters::GraphicsOperations MakeGuestGraphicsOperations(GuestGraphicsOperationsContext& context) {
    return {
        .context = &context,
        .available = [](void* opaque) { return Binding(opaque).engine->Available(); },
        .get_info = [](void* opaque,
                       micropixel_graphics_info_t& info) { return Binding(opaque).engine->GetInfo(info); },
        .submit =
            [](void* opaque, const uint8_t* bytes, uint32_t length, const device::TextureAccess& textures) {
                return Binding(opaque).engine->Submit(bytes, length, textures);
            },
        .load_font = [](void* opaque, const device::FontResourceView& resource,
                        micropixel_font_info_t& info) { return Binding(opaque).engine->LoadFont(resource, info); },
        .release_font = [](void* opaque,
                           micropixel_font_handle_t font) { return Binding(opaque).engine->ReleaseFont(font); },
        .measure_text =
            [](void* opaque, micropixel_font_handle_t font, const char* text, uint32_t text_length,
               micropixel_text_metrics_t& metrics) {
                return Binding(opaque).engine->MeasureText(font, text, text_length, metrics);
            },
        .begin_bitmap_update_frame = [](void* opaque) { return Binding(opaque).engine->BeginBitmapUpdateFrame(); },
        .update_bitmap =
            [](void* opaque, const device::BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
               const uint8_t* pixels, uint32_t stride) {
                return Binding(opaque).engine->UpdateBitmap(bitmap, x, y, width, height, pixels, stride);
            },
        .commit_bitmap_update_frame = [](void* opaque) { return Binding(opaque).engine->CommitBitmapUpdateFrame(); },
        .scale_bitmap =
            [](void* opaque, const device::BitmapView& source, const device::BitmapView& destination) {
                return ScaleBitmap(Binding(opaque), source, destination);
            },
        .show_launch_bitmap =
            [](void* opaque, const device::BitmapView& bitmap) {
                GuestGraphicsOperationsContext& binding = Binding(opaque);
                return binding.hooks.show_launch_bitmap(binding.hooks.context, bitmap);
            },
        .dismiss_launch_bitmap =
            [](void* opaque) {
                GuestGraphicsOperationsContext& binding = Binding(opaque);
                binding.hooks.dismiss_launch_bitmap(binding.hooks.context);
            },
        .release_guest_resources = [](void* opaque) { Binding(opaque).engine->Release(); },
    };
}

}  // namespace micropixel::platform::lvgl
