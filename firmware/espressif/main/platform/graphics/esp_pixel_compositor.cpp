#include "platform/graphics/esp_pixel_compositor.hpp"

#include <cstdint>

#include "esp_timer.h"

namespace micropixel::platform::graphics {
namespace {

#if defined(CONFIG_IDF_TARGET_ESP32S31) && CONFIG_IDF_TARGET_ESP32S31
constexpr uint32_t kMinimumPpaBlendPixels = 512U;
#else
constexpr uint32_t kMinimumPpaBlendPixels = 0U;
#endif

constexpr std::array<uint64_t, kPpaBlendHistogramBinCount - 1U> kPpaBlendHistogramUpperBounds{
    256U, 512U, 840U, 1024U, 2048U, 4096U, 16384U,
};

uint32_t MinimumPpaBlendPixels(uint32_t configured_minimum) {
    return configured_minimum > kMinimumPpaBlendPixels ? configured_minimum : kMinimumPpaBlendPixels;
}

std::size_t PpaBlendHistogramBin(uint64_t pixels) {
    for (std::size_t index = 0U; index < kPpaBlendHistogramUpperBounds.size(); ++index) {
        if (pixels < kPpaBlendHistogramUpperBounds[index]) {
            return index;
        }
    }
    return kPpaBlendHistogramBinCount - 1U;
}

uint32_t BytesPerPixel(SurfacePixelFormat format) {
    if (format == SurfacePixelFormat::kBgr888) {
        return 3U;
    }
    if (format == SurfacePixelFormat::kBgra8888) {
        return 4U;
    }
    return format == SurfacePixelFormat::kRgb565 ? 2U : 0U;
}

template <typename Surface>
bool ValidSurface(const Surface& surface) {
    const uint32_t bytes_per_pixel = BytesPerPixel(surface.format);
    if (surface.pixels == nullptr || surface.width == 0U || surface.height == 0U || bytes_per_pixel == 0U ||
        surface.width > UINT32_MAX - surface.origin_x ||
        surface.origin_x + surface.width > UINT32_MAX / bytes_per_pixel) {
        return false;
    }
    const uint32_t row_bytes = (surface.origin_x + surface.width) * bytes_per_pixel;
    const uint64_t required =
        static_cast<uint64_t>(surface.stride) * (surface.origin_y + surface.height - 1U) + row_bytes;
    return surface.stride >= row_bytes && required <= surface.size;
}

bool ValidDestination(PixelSurface surface) {
    return ValidSurface(surface) && surface.format != SurfacePixelFormat::kBgra8888;
}

bool ValidSourceRect(ConstPixelSurface source, SurfaceRect rect) {
    return rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0 &&
           static_cast<int64_t>(rect.x) + rect.width <= source.width &&
           static_cast<int64_t>(rect.y) + rect.height <= source.height;
}

bool ValidDestinationRect(SurfaceRect rect) {
    const int64_t right = static_cast<int64_t>(rect.x) + rect.width;
    const int64_t bottom = static_cast<int64_t>(rect.y) + rect.height;
    return rect.width > 0 && rect.height > 0 && right >= INT32_MIN && right <= INT32_MAX && bottom >= INT32_MIN &&
           bottom <= INT32_MAX;
}

bool EntirelyInside(PixelSurface surface, SurfaceRect rect) {
    return rect.x >= 0 && rect.y >= 0 && static_cast<int64_t>(rect.x) + rect.width <= surface.width &&
           static_cast<int64_t>(rect.y) + rect.height <= surface.height;
}

uint32_t PhysicalWidth(uint32_t stride, SurfacePixelFormat format) { return stride / BytesPerPixel(format); }

uint32_t PhysicalHeight(uint32_t size, uint32_t stride) { return stride == 0U ? 0U : size / stride; }

uint64_t Pixels(SurfaceRect rect) { return static_cast<uint64_t>(rect.width) * rect.height; }

bool ExactPpaScale(uint32_t source, uint32_t destination, float& scale) {
    constexpr uint32_t kFractionSteps = 16U;
    const uint64_t quantized = (static_cast<uint64_t>(destination) * kFractionSteps + source - 1U) / source;
    if (quantized == 0U || quantized >= 256U * kFractionSteps ||
        static_cast<uint64_t>(source) * quantized / kFractionSteps != destination) {
        return false;
    }
    scale = static_cast<float>(quantized) / static_cast<float>(kFractionSteps);
    return true;
}

ppa_srm_color_mode_t SrmMode(SurfacePixelFormat format) {
    return format == SurfacePixelFormat::kRgb565 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888;
}

ppa_blend_color_mode_t BlendMode(SurfacePixelFormat format) {
    if (format == SurfacePixelFormat::kRgb565) {
        return PPA_BLEND_COLOR_MODE_RGB565;
    }
    return format == SurfacePixelFormat::kBgra8888 ? PPA_BLEND_COLOR_MODE_ARGB8888 : PPA_BLEND_COLOR_MODE_RGB888;
}

}  // namespace

EspPixelCompositor::~EspPixelCompositor() { Release(); }

esp_err_t EspPixelCompositor::Initialize(uint32_t minimum_hardware_pixels) {
    minimum_hardware_pixels_ = minimum_hardware_pixels == 0U ? 1U : minimum_hardware_pixels;
    esp_err_t first_error = ESP_OK;
    auto register_client = [&first_error](ppa_operation_t type, ppa_client_handle_t& client) {
        if (client != nullptr) {
            return;
        }
        ppa_client_config_t config{
            .oper_type = type,
            .max_pending_trans_num = 1U,
            .data_burst_length = PPA_DATA_BURST_LENGTH_128,
            .flags = {},
        };
        const esp_err_t status = ppa_register_client(&config, &client);
        if (status != ESP_OK && first_error == ESP_OK) {
            first_error = status;
        }
    };
    register_client(PPA_OPERATION_FILL, fill_client_);
    register_client(PPA_OPERATION_SRM, scale_client_);
    register_client(PPA_OPERATION_BLEND, blend_client_);
    if (dma2d_client_ == nullptr) {
        async_color_convert_config_t config{};
        config.backlog = 1U;
        config.dma_burst_size = 128U;
        const esp_err_t status = esp_async_color_convert_install_dma2d(&config, &dma2d_client_);
        if (status != ESP_OK && first_error == ESP_OK) {
            first_error = status;
        }
    }
    return first_error;
}

void EspPixelCompositor::Release() {
    if (dma2d_client_ != nullptr) {
        (void)esp_async_color_convert_uninstall(dma2d_client_);
        dma2d_client_ = nullptr;
    }
    if (blend_client_ != nullptr) {
        (void)ppa_unregister_client(blend_client_);
        blend_client_ = nullptr;
    }
    if (scale_client_ != nullptr) {
        (void)ppa_unregister_client(scale_client_);
        scale_client_ = nullptr;
    }
    if (fill_client_ != nullptr) {
        (void)ppa_unregister_client(fill_client_);
        fill_client_ = nullptr;
    }
}

bool EspPixelCompositor::TryFill(PixelSurface destination, SurfaceRect rect, uint32_t rgb888) {
    if (fill_client_ == nullptr || !EntirelyInside(destination, rect) || Pixels(rect) < minimum_hardware_pixels_) {
        return false;
    }
    ppa_fill_oper_config_t config{};
    config.out.buffer = destination.pixels;
    config.out.buffer_size = destination.size;
    config.out.pic_w = PhysicalWidth(destination.stride, destination.format);
    config.out.pic_h = PhysicalHeight(destination.size, destination.stride);
    config.out.block_offset_x = destination.origin_x + static_cast<uint32_t>(rect.x);
    config.out.block_offset_y = destination.origin_y + static_cast<uint32_t>(rect.y);
    config.out.fill_cm =
        destination.format == SurfacePixelFormat::kRgb565 ? PPA_FILL_COLOR_MODE_RGB565 : PPA_FILL_COLOR_MODE_RGB888;
    config.fill_block_w = static_cast<uint32_t>(rect.width);
    config.fill_block_h = static_cast<uint32_t>(rect.height);
    config.fill_argb_color.val = 0xff000000U | rgb888;
    config.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_fill(fill_client_, &config) == ESP_OK;
}

bool EspPixelCompositor::Fill(PixelSurface destination, SurfaceRect rect, uint32_t rgb888, uint8_t opacity) {
    if (!ValidDestination(destination) || !ValidDestinationRect(rect) || (rgb888 & 0xff000000U) != 0U) {
        return false;
    }
    if (opacity == 255U && TryFill(destination, rect, rgb888)) {
        ++stats_.ppa_fills;
        return true;
    }
    ++stats_.software_fallbacks;
    return software_.Fill(destination, rect, rgb888, opacity);
}

bool EspPixelCompositor::TryDma2dCopy(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                                      SurfaceRect destination_rect) {
    if (dma2d_client_ == nullptr || source.format != SurfacePixelFormat::kBgr888 ||
        destination.format != SurfacePixelFormat::kBgr888 || source_rect.width != destination_rect.width ||
        source_rect.height != destination_rect.height || !EntirelyInside(destination, destination_rect) ||
        Pixels(destination_rect) < minimum_hardware_pixels_) {
        return false;
    }
    async_color_convert_request_t copy{};
    copy.src_buffer = source.pixels;
    copy.src_stride = PhysicalWidth(source.stride, source.format);
    copy.src_height = PhysicalHeight(source.size, source.stride);
    copy.src_x = source.origin_x + static_cast<uint32_t>(source_rect.x);
    copy.src_y = source.origin_y + static_cast<uint32_t>(source_rect.y);
    copy.dst_buffer = destination.pixels;
    copy.dst_stride = PhysicalWidth(destination.stride, destination.format);
    copy.dst_height = PhysicalHeight(destination.size, destination.stride);
    copy.dst_x = destination.origin_x + static_cast<uint32_t>(destination_rect.x);
    copy.dst_y = destination.origin_y + static_cast<uint32_t>(destination_rect.y);
    copy.copy_width = static_cast<uint32_t>(destination_rect.width);
    copy.copy_height = static_cast<uint32_t>(destination_rect.height);
    copy.src_color_format = ESP_COLOR_FOURCC_BGR24;
    copy.dst_color_format = ESP_COLOR_FOURCC_BGR24;
    return esp_color_convert_blocking(dma2d_client_, &copy, -1) == ESP_OK;
}

bool EspPixelCompositor::TryScale(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                                  SurfaceRect destination_rect) {
    if (scale_client_ == nullptr || source.format != SurfacePixelFormat::kBgr888 ||
        destination.format != SurfacePixelFormat::kBgr888 || !EntirelyInside(destination, destination_rect) ||
        Pixels(destination_rect) < minimum_hardware_pixels_) {
        return false;
    }
    float scale_x = 0.0F;
    float scale_y = 0.0F;
    if (!ExactPpaScale(static_cast<uint32_t>(source_rect.width), static_cast<uint32_t>(destination_rect.width),
                       scale_x) ||
        !ExactPpaScale(static_cast<uint32_t>(source_rect.height), static_cast<uint32_t>(destination_rect.height),
                       scale_y)) {
        return false;
    }
    ppa_srm_oper_config_t config{};
    config.in.buffer = source.pixels;
    config.in.pic_w = PhysicalWidth(source.stride, source.format);
    config.in.pic_h = PhysicalHeight(source.size, source.stride);
    config.in.block_offset_x = source.origin_x + static_cast<uint32_t>(source_rect.x);
    config.in.block_offset_y = source.origin_y + static_cast<uint32_t>(source_rect.y);
    config.in.block_w = static_cast<uint32_t>(source_rect.width);
    config.in.block_h = static_cast<uint32_t>(source_rect.height);
    config.in.srm_cm = SrmMode(source.format);
    config.out.buffer = destination.pixels;
    config.out.buffer_size = destination.size;
    config.out.pic_w = PhysicalWidth(destination.stride, destination.format);
    config.out.pic_h = PhysicalHeight(destination.size, destination.stride);
    config.out.block_offset_x = destination.origin_x + static_cast<uint32_t>(destination_rect.x);
    config.out.block_offset_y = destination.origin_y + static_cast<uint32_t>(destination_rect.y);
    config.out.srm_cm = SrmMode(destination.format);
    config.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    config.scale_x = scale_x;
    config.scale_y = scale_y;
    config.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(scale_client_, &config) == ESP_OK;
}

bool EspPixelCompositor::TryBlend(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                                  SurfaceRect destination_rect, uint8_t opacity) {
    if (blend_client_ == nullptr || destination.format != SurfacePixelFormat::kBgr888 ||
        (source.format != SurfacePixelFormat::kBgr888 && source.format != SurfacePixelFormat::kBgra8888) ||
        source_rect.width != destination_rect.width || source_rect.height != destination_rect.height ||
        !EntirelyInside(destination, destination_rect) ||
        Pixels(destination_rect) < MinimumPpaBlendPixels(minimum_hardware_pixels_)) {
        return false;
    }
    ppa_blend_oper_config_t config{};
    config.in_bg.buffer = destination.pixels;
    config.in_bg.pic_w = PhysicalWidth(destination.stride, destination.format);
    config.in_bg.pic_h = PhysicalHeight(destination.size, destination.stride);
    config.in_bg.block_w = static_cast<uint32_t>(destination_rect.width);
    config.in_bg.block_h = static_cast<uint32_t>(destination_rect.height);
    config.in_bg.block_offset_x = destination.origin_x + static_cast<uint32_t>(destination_rect.x);
    config.in_bg.block_offset_y = destination.origin_y + static_cast<uint32_t>(destination_rect.y);
    config.in_bg.blend_cm = BlendMode(destination.format);
    config.in_fg.buffer = source.pixels;
    config.in_fg.pic_w = PhysicalWidth(source.stride, source.format);
    config.in_fg.pic_h = PhysicalHeight(source.size, source.stride);
    config.in_fg.block_w = static_cast<uint32_t>(source_rect.width);
    config.in_fg.block_h = static_cast<uint32_t>(source_rect.height);
    config.in_fg.block_offset_x = source.origin_x + static_cast<uint32_t>(source_rect.x);
    config.in_fg.block_offset_y = source.origin_y + static_cast<uint32_t>(source_rect.y);
    config.in_fg.blend_cm = BlendMode(source.format);
    config.out.buffer = destination.pixels;
    config.out.buffer_size = destination.size;
    config.out.pic_w = PhysicalWidth(destination.stride, destination.format);
    config.out.pic_h = PhysicalHeight(destination.size, destination.stride);
    config.out.block_offset_x = destination.origin_x + static_cast<uint32_t>(destination_rect.x);
    config.out.block_offset_y = destination.origin_y + static_cast<uint32_t>(destination_rect.y);
    config.out.blend_cm = BlendMode(destination.format);
    config.bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    config.bg_alpha_fix_val = UINT8_MAX;
    if (source.format == SurfacePixelFormat::kBgra8888) {
        if (opacity != 255U) {
            return false;
        }
        config.fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    } else {
        config.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
        config.fg_alpha_fix_val = opacity;
    }
    config.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_blend(blend_client_, &config) == ESP_OK;
}

bool EspPixelCompositor::Blit(ConstPixelSurface source, SurfaceRect source_rect, PixelSurface destination,
                              SurfaceRect destination_rect, uint8_t opacity) {
    if (!ValidSurface(source) || !ValidDestination(destination) || !ValidSourceRect(source, source_rect) ||
        !ValidDestinationRect(destination_rect)) {
        return false;
    }
    if (opacity == 0U) {
        return true;
    }
    const bool same_size = source_rect.width == destination_rect.width && source_rect.height == destination_rect.height;
    if (opacity == 255U && source.format == SurfacePixelFormat::kBgr888 && same_size &&
        TryDma2dCopy(source, source_rect, destination, destination_rect)) {
        ++stats_.dma2d_copies;
        return true;
    }
    if (source.format == SurfacePixelFormat::kBgr888 && !same_size && opacity == 255U &&
        TryScale(source, source_rect, destination, destination_rect)) {
        ++stats_.ppa_scales;
        return true;
    }
    const bool blend_candidate = (source.format == SurfacePixelFormat::kBgra8888 || opacity != 255U) && same_size;
    const uint64_t blend_pixels = blend_candidate ? Pixels(destination_rect) : 0U;
    if (blend_candidate && blend_pixels < kMinimumPpaBlendPixels) {
        ++stats_.small_blend_candidates;
    }
    const int64_t blend_started_us = blend_candidate ? esp_timer_get_time() : 0;
    if (blend_candidate && TryBlend(source, source_rect, destination, destination_rect, opacity)) {
        const uint64_t blend_elapsed_us = static_cast<uint64_t>(esp_timer_get_time() - blend_started_us);
        const std::size_t histogram_bin = PpaBlendHistogramBin(blend_pixels);
        ++stats_.ppa_blends;
        stats_.ppa_blend_pixels += blend_pixels;
        stats_.ppa_blend_elapsed_us += blend_elapsed_us;
        ++stats_.ppa_blend_histogram_calls[histogram_bin];
        stats_.ppa_blend_histogram_elapsed_us[histogram_bin] += blend_elapsed_us;
        return true;
    }
    ++stats_.software_fallbacks;
    return software_.Blit(source, source_rect, destination, destination_rect, opacity);
}

}  // namespace micropixel::platform::graphics
