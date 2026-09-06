#include "host/ui/lvgl/square_common/hall_cover_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "lvgl.h"
#include "platform/lvgl/display/jpeg_cover_decoder.hpp"
#include "platform/lvgl/display/png_cover_decoder.hpp"

namespace micropixel::host_ui::lvgl::square_common {
namespace {

void ScaleRgb888Cover(const host_ui::HallCoverModel& source, uint32_t target_size, uint8_t* destination) {
    const uint32_t crop_size = std::min(source.width, source.height);
    const uint32_t crop_x = (source.width - crop_size) / 2U;
    const uint32_t crop_y = (source.height - crop_size) / 2U;
    const uint32_t destination_stride = HallCoverStride(target_size);
    for (uint32_t y = 0U; y < target_size; ++y) {
        const uint32_t source_y = crop_y + static_cast<uint32_t>(static_cast<uint64_t>(y) * crop_size / target_size);
        uint8_t* destination_row = destination + static_cast<size_t>(y) * destination_stride;
        for (uint32_t x = 0U; x < target_size; ++x) {
            const uint32_t source_x =
                crop_x + static_cast<uint32_t>(static_cast<uint64_t>(x) * crop_size / target_size);
            std::memcpy(
                destination_row + static_cast<size_t>(x) * 3U,
                source.data + static_cast<size_t>(source_y) * source.stride + static_cast<size_t>(source_x) * 3U, 3U);
        }
    }
}

}  // namespace

uint32_t HallCoverStride(uint32_t size) { return LV_DRAW_BUF_STRIDE(size, LV_COLOR_FORMAT_RGB888); }

uint32_t HallCoverBytes(uint32_t size) { return HallCoverStride(size) * size; }

bool DecodeHallCoverRgb888(const host_ui::HallCoverModel& source, uint32_t target_size, uint32_t corner_radius,
                           uint32_t top_background_rgb, uint8_t* destination) {
    if (source.data == nullptr || source.size == 0U || source.width == 0U || source.height == 0U || target_size == 0U ||
        destination == nullptr) {
        return false;
    }
    const uint32_t destination_stride = HallCoverStride(target_size);
    bool decoded = false;
    if (source.format == host_ui::HallCoverFormat::kJpeg) {
        decoded = platform::lvgl::DecodeJpegCoverRgb888(source.data, source.size, source.width, source.height,
                                                        destination, target_size, target_size, destination_stride);
    } else if (source.format == host_ui::HallCoverFormat::kPng) {
        decoded =
            platform::lvgl::DecodePngCoverRgb888(source.data, source.size, source.width, source.height, destination,
                                                 target_size, target_size, destination_stride, top_background_rgb);
    } else if (source.stride >= source.width * 3U && source.size >= source.stride * source.height) {
        ScaleRgb888Cover(source, target_size, destination);
        decoded = true;
    }
    if (decoded) {
        MaskHallCoverRgb888(destination, target_size, destination_stride, corner_radius, top_background_rgb);
    }
    return decoded;
}

}  // namespace micropixel::host_ui::lvgl::square_common
