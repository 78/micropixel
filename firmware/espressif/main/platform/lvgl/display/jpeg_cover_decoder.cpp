#include "platform/lvgl/display/jpeg_cover_decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "soc/soc_caps.h"
#if SOC_JPEG_DECODE_SUPPORTED
#include "driver/jpeg_decode.h"
#endif
#include "sdkconfig.h"

namespace micropixel::platform::lvgl {
namespace {

constexpr uint32_t kMaximumSourceDimension = 8192U;
constexpr int kDecodeTimeoutMs = 100;

#if SOC_JPEG_DECODE_SUPPORTED
uint32_t AlignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1U) / alignment * alignment; }

bool PaddedDimensions(const jpeg_decode_picture_info_t& info, uint32_t& width_out, uint32_t& height_out) {
    uint32_t mcu_width = 0U;
    uint32_t mcu_height = 0U;
    switch (info.sample_method) {
        case JPEG_DOWN_SAMPLING_YUV444:
            mcu_width = 8U;
            mcu_height = 8U;
            break;
        case JPEG_DOWN_SAMPLING_YUV422:
            mcu_width = 16U;
            mcu_height = 8U;
            break;
        case JPEG_DOWN_SAMPLING_YUV420:
            mcu_width = 16U;
            mcu_height = 16U;
            break;
        default:
            return false;
    }
    width_out = AlignUp(info.width, mcu_width);
    height_out = AlignUp(info.height, mcu_height);
    return true;
}
#endif

void ScaleCenterCrop(const uint8_t* source, uint32_t source_width, uint32_t source_height, uint32_t source_stride,
                     uint8_t* destination, uint32_t target_width, uint32_t target_height, uint32_t target_stride,
                     bool source_is_rgb) {
    uint32_t crop_width = source_width;
    uint32_t crop_height = source_height;
    if (static_cast<uint64_t>(source_width) * target_height > static_cast<uint64_t>(source_height) * target_width) {
        crop_width = static_cast<uint32_t>(static_cast<uint64_t>(source_height) * target_width / target_height);
    } else {
        crop_height = static_cast<uint32_t>(static_cast<uint64_t>(source_width) * target_height / target_width);
    }
    const uint32_t crop_x = (source_width - crop_width) / 2U;
    const uint32_t crop_y = (source_height - crop_height) / 2U;
    for (uint32_t target_y = 0U; target_y < target_height; ++target_y) {
        const uint32_t source_y =
            crop_y + static_cast<uint32_t>(static_cast<uint64_t>(target_y) * crop_height / target_height);
        uint8_t* destination_row = destination + static_cast<size_t>(target_y) * target_stride;
        const uint8_t* source_row = source + static_cast<size_t>(source_y) * source_stride;
        for (uint32_t target_x = 0U; target_x < target_width; ++target_x) {
            const uint32_t source_x =
                crop_x + static_cast<uint32_t>(static_cast<uint64_t>(target_x) * crop_width / target_width);
            uint8_t* destination_pixel = destination_row + static_cast<size_t>(target_x) * 3U;
            const uint8_t* source_pixel = source_row + static_cast<size_t>(source_x) * 3U;
            if (source_is_rgb) {
                destination_pixel[0] = source_pixel[2];
                destination_pixel[1] = source_pixel[1];
                destination_pixel[2] = source_pixel[0];
            } else {
                std::memcpy(destination_pixel, source_pixel, 3U);
            }
        }
    }
}

}  // namespace

bool DecodeJpegCoverRgb888(const uint8_t* source, uint32_t source_size, uint32_t declared_width,
                           uint32_t declared_height, uint8_t* destination, uint32_t target_width,
                           uint32_t target_height, uint32_t target_stride) {
    if (source == nullptr || source_size == 0U || destination == nullptr || target_width == 0U || target_height == 0U ||
        target_width > UINT32_MAX / 3U || target_stride < target_width * 3U) {
        return false;
    }

#if SOC_JPEG_DECODE_SUPPORTED
    jpeg_decode_picture_info_t info{};
    uint32_t decoded_width = 0U;
    uint32_t decoded_height = 0U;
    if (jpeg_decoder_get_info(source, source_size, &info) != ESP_OK || info.width != declared_width ||
        info.height != declared_height || info.width == 0U || info.height == 0U ||
        info.width > kMaximumSourceDimension || info.height > kMaximumSourceDimension ||
        !PaddedDimensions(info, decoded_width, decoded_height)) {
        return false;
    }
    const uint64_t required_output_size = static_cast<uint64_t>(decoded_width) * decoded_height * 3U;
    if (required_output_size == 0U || required_output_size > UINT32_MAX) {
        return false;
    }

    const jpeg_decode_memory_alloc_cfg_t input_config{.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    const jpeg_decode_memory_alloc_cfg_t output_config{.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    size_t input_allocation_size = 0U;
    size_t output_allocation_size = 0U;
    auto* input = static_cast<uint8_t*>(jpeg_alloc_decoder_mem(source_size, &input_config, &input_allocation_size));
    auto* output = static_cast<uint8_t*>(
        jpeg_alloc_decoder_mem(static_cast<size_t>(required_output_size), &output_config, &output_allocation_size));
    if (input == nullptr || output == nullptr || input_allocation_size < source_size ||
        output_allocation_size < required_output_size || output_allocation_size > UINT32_MAX) {
        std::free(input);
        std::free(output);
        return false;
    }
    std::memcpy(input, source, source_size);

    const jpeg_decode_engine_cfg_t engine_config{
        .intr_priority = 0,
        .timeout_ms = kDecodeTimeoutMs,
        .flags = {.allow_pd = 1U},
    };
    jpeg_decoder_handle_t decoder = nullptr;
    if (jpeg_new_decoder_engine(&engine_config, &decoder) != ESP_OK) {
        std::free(input);
        std::free(output);
        return false;
    }
    const jpeg_decode_cfg_t decode_config{.output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
                                          .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
                                          .conv_std = JPEG_YUV_RGB_CONV_STD_BT601};
    uint32_t output_size = 0U;
    const bool decoded = jpeg_decoder_process(decoder, &decode_config, input, source_size, output,
                                              static_cast<uint32_t>(output_allocation_size), &output_size) == ESP_OK &&
                         output_size == required_output_size;
    if (decoded) {
        ScaleCenterCrop(output, info.width, info.height, decoded_width * 3U, destination, target_width, target_height,
                        target_stride, false);
    }
    (void)jpeg_del_decoder_engine(decoder);
    std::free(input);
    std::free(output);
    return decoded;
#else
    if (source_size > static_cast<uint32_t>(std::numeric_limits<int>::max()) || declared_width > UINT16_MAX ||
        declared_height > UINT16_MAX) {
        return false;
    }
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB888;
    jpeg_dec_handle_t decoder = nullptr;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK) {
        return false;
    }
    jpeg_dec_io_t io{};
    io.inbuf = const_cast<uint8_t*>(source);
    io.inbuf_len = static_cast<int>(source_size);
    jpeg_dec_header_info_t info{};
    bool decoded = jpeg_dec_parse_header(decoder, &io, &info) == JPEG_ERR_OK && info.width == declared_width &&
                   info.height == declared_height && info.width != 0U && info.height != 0U;
    const uint64_t output_bytes = static_cast<uint64_t>(info.width) * info.height * 3U;
    decoded = decoded && output_bytes <= UINT32_MAX;
    uint8_t* output = nullptr;
    if (decoded) {
        output = static_cast<uint8_t*>(
            heap_caps_aligned_calloc(16U, static_cast<size_t>(output_bytes), 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        decoded = output != nullptr;
    }
    if (decoded) {
        io.outbuf = output;
        decoded = jpeg_dec_process(decoder, &io) == JPEG_ERR_OK;
    }
    if (decoded) {
        ScaleCenterCrop(output, info.width, info.height, static_cast<uint32_t>(info.width) * 3U, destination,
                        target_width, target_height, target_stride, true);
    }
    heap_caps_free(output);
    (void)jpeg_dec_close(decoder);
    return decoded;
#endif
}

}  // namespace micropixel::platform::lvgl
