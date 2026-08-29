#include "host/ui/lvgl/square_common/hall_cover_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

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

void MaskHallCoverRgb888(uint8_t* destination, uint32_t size, uint32_t radius, uint32_t top_background_rgb,
                         uint32_t bottom_background_rgb) {
    if (destination == nullptr || size == 0U || radius <= 1U) {
        return;
    }
    radius = std::min(radius, size / 2U);
    const uint32_t stride = HallCoverStride(size);
    const int32_t center = static_cast<int32_t>(radius - 1U);
    const int32_t radius_squared = center * center;
    const auto set_pixel = [destination, stride](uint32_t x, uint32_t y, uint32_t rgb) {
        uint8_t* pixel = destination + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 3U;
        pixel[0] = static_cast<uint8_t>(rgb);
        pixel[1] = static_cast<uint8_t>(rgb >> 8U);
        pixel[2] = static_cast<uint8_t>(rgb >> 16U);
    };
    for (uint32_t y = 0U; y < radius; ++y) {
        for (uint32_t x = 0U; x < radius; ++x) {
            const int32_t dx = static_cast<int32_t>(x) - center;
            const int32_t dy = static_cast<int32_t>(y) - center;
            if (dx * dx + dy * dy <= radius_squared) {
                continue;
            }
            set_pixel(x, y, top_background_rgb);
            set_pixel(size - 1U - x, y, top_background_rgb);
            set_pixel(x, size - 1U - y, bottom_background_rgb);
            set_pixel(size - 1U - x, size - 1U - y, bottom_background_rgb);
        }
    }
}

bool DecodeHallCoverRgb888(const host_ui::HallCoverModel& source, uint32_t target_size, uint32_t corner_radius,
                           uint32_t top_background_rgb, uint32_t bottom_background_rgb, uint8_t* destination) {
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
        MaskHallCoverRgb888(destination, target_size, corner_radius, top_background_rgb, bottom_background_rgb);
    }
    return decoded;
}

}  // namespace micropixel::host_ui::lvgl::square_common
