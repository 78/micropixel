#include "platform/lvgl/display/png_cover_decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"
#include "png.h"

namespace micropixel::platform::lvgl {
namespace {

constexpr uint32_t kMaximumSourceDimension = 8192U;

struct MemoryReader final {
    const uint8_t* data{};
    size_t size{};
    size_t offset{};
};

void ReadPngBytes(png_structp png, png_bytep output, png_size_t size) {
    auto* reader = static_cast<MemoryReader*>(png_get_io_ptr(png));
    if (reader == nullptr || reader->offset > reader->size || size > reader->size - reader->offset) {
        png_error(png, "truncated PNG cover");
        return;
    }
    std::memcpy(output, reader->data + reader->offset, size);
    reader->offset += size;
}

void PngError(png_structp png, png_const_charp) { png_longjmp(png, 1); }

void PngWarning(png_structp, png_const_charp) {}

png_voidp PngPsramAlloc(png_structp, png_alloc_size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void PngPsramFree(png_structp, png_voidp memory) { heap_caps_free(memory); }

}  // namespace

bool DecodePngCoverRgb888(const uint8_t* source, uint32_t source_size, uint32_t declared_width,
                          uint32_t declared_height, uint8_t* destination, uint32_t target_width, uint32_t target_height,
                          uint32_t target_stride, uint32_t background_rgb888) {
    if (source == nullptr || source_size == 0U || destination == nullptr || target_width == 0U || target_height == 0U ||
        target_stride != target_width * 3U) {
        return false;
    }

    png_structp png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, nullptr, PngError, PngWarning, nullptr,
                                               PngPsramAlloc, PngPsramFree);
    if (png == nullptr) {
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        return false;
    }

    volatile uint8_t* row = nullptr;
    if (setjmp(png_jmpbuf(png)) != 0) {
        heap_caps_free(const_cast<uint8_t*>(row));
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }

    MemoryReader reader{source, source_size, 0U};
    png_set_read_fn(png, &reader, ReadPngBytes);
    png_set_user_limits(png, kMaximumSourceDimension, kMaximumSourceDimension);
    png_read_info(png, info);

    const uint32_t source_width = png_get_image_width(png, info);
    const uint32_t source_height = png_get_image_height(png, info);
    int color_type = png_get_color_type(png, info);
    const int bit_depth = png_get_bit_depth(png, info);
    if (source_width == 0U || source_height == 0U || source_width != declared_width ||
        source_height != declared_height || png_get_interlace_type(png, info) != PNG_INTERLACE_NONE) {
        png_error(png, "unsupported PNG cover dimensions or interlace");
    }
    if (bit_depth == 16) {
        png_set_strip_16(png);
    }
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS) != 0U) {
        png_set_tRNS_to_alpha(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    png_set_bgr(png);
    png_read_update_info(png, info);

    const uint32_t channels = png_get_channels(png, info);
    const size_t row_bytes = png_get_rowbytes(png, info);
    if ((channels != 3U && channels != 4U) || row_bytes != static_cast<size_t>(source_width) * channels) {
        png_error(png, "unsupported PNG cover pixel layout");
    }
    row = static_cast<uint8_t*>(heap_caps_aligned_alloc(64U, row_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (row == nullptr) {
        png_error(png, "PNG cover row allocation failed");
    }

    uint32_t crop_width = source_width;
    uint32_t crop_height = source_height;
    if (static_cast<uint64_t>(source_width) * target_height > static_cast<uint64_t>(source_height) * target_width) {
        crop_width = static_cast<uint32_t>(static_cast<uint64_t>(source_height) * target_width / target_height);
    } else {
        crop_height = static_cast<uint32_t>(static_cast<uint64_t>(source_width) * target_height / target_width);
    }
    const uint32_t crop_x = (source_width - crop_width) / 2U;
    const uint32_t crop_y = (source_height - crop_height) / 2U;
    const uint8_t background_blue = static_cast<uint8_t>(background_rgb888);
    const uint8_t background_green = static_cast<uint8_t>(background_rgb888 >> 8U);
    const uint8_t background_red = static_cast<uint8_t>(background_rgb888 >> 16U);

    uint32_t target_y = 0U;
    for (uint32_t source_y = 0U; source_y < source_height; ++source_y) {
        png_read_row(png, const_cast<uint8_t*>(row), nullptr);
        while (target_y < target_height &&
               crop_y + static_cast<uint32_t>(static_cast<uint64_t>(target_y) * crop_height / target_height) ==
                   source_y) {
            uint8_t* destination_row = destination + static_cast<size_t>(target_y) * target_stride;
            for (uint32_t target_x = 0U; target_x < target_width; ++target_x) {
                const uint32_t source_x =
                    crop_x + static_cast<uint32_t>(static_cast<uint64_t>(target_x) * crop_width / target_width);
                const uint8_t* source_pixel =
                    const_cast<const uint8_t*>(row) + static_cast<size_t>(source_x) * channels;
                uint8_t* destination_pixel = destination_row + static_cast<size_t>(target_x) * 3U;
                if (channels == 3U || source_pixel[3] == UINT8_MAX) {
                    std::memcpy(destination_pixel, source_pixel, 3U);
                } else {
                    const uint32_t alpha = source_pixel[3];
                    const uint32_t inverse_alpha = UINT8_MAX - alpha;
                    destination_pixel[0] = static_cast<uint8_t>(
                        (source_pixel[0] * alpha + background_blue * inverse_alpha + 127U) / UINT8_MAX);
                    destination_pixel[1] = static_cast<uint8_t>(
                        (source_pixel[1] * alpha + background_green * inverse_alpha + 127U) / UINT8_MAX);
                    destination_pixel[2] = static_cast<uint8_t>(
                        (source_pixel[2] * alpha + background_red * inverse_alpha + 127U) / UINT8_MAX);
                }
            }
            ++target_y;
        }
    }
    png_read_end(png, info);
    const bool complete = target_y == target_height;
    heap_caps_free(const_cast<uint8_t*>(row));
    png_destroy_read_struct(&png, &info, nullptr);
    return complete;
}

}  // namespace micropixel::platform::lvgl
