#include "runtime/resources/bitmap_decoder.hpp"

#include <cstdlib>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "png.h"
#include "sdkconfig.h"

namespace micropixel::runtime {
namespace {

bool ValidDecodedSize(uint32_t width, uint32_t height, uint32_t bytes_per_pixel, size_t size) {
    return width > 0U && height > 0U && width <= 720U && height <= 720U &&
           (bytes_per_pixel == 3U || bytes_per_pixel == 4U) &&
           size == static_cast<size_t>(width) * height * bytes_per_pixel &&
           size <= CONFIG_MICROPIXEL_BITMAP_PSRAM_QUOTA_BYTES;
}

void RgbToLvRgb888(uint8_t* data, uint32_t size) {
    for (uint32_t offset = 0U; offset + 2U < size; offset += 3U) {
        uint8_t red = data[offset];
        data[offset] = data[offset + 2U];
        data[offset + 2U] = red;
    }
}

bool DecodeJpeg(const micropixel_bundle_asset_view_t& asset, device::BitmapView& view) {
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB888;
    jpeg_dec_handle_t decoder = nullptr;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK) {
        return false;
    }

    jpeg_dec_io_t io{};
    io.inbuf = const_cast<uint8_t*>(asset.data);
    io.inbuf_len = static_cast<int>(asset.size);
    jpeg_dec_header_info_t header{};
    bool succeeded = false;
    if (jpeg_dec_parse_header(decoder, &io, &header) == JPEG_ERR_OK) {
        size_t output_size = static_cast<size_t>(header.width) * header.height * 3U;
        if (ValidDecodedSize(header.width, header.height, 3U, output_size)) {
            auto* output =
                static_cast<uint8_t*>(heap_caps_aligned_alloc(64U, output_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (output != nullptr) {
                io.outbuf = output;
                if (jpeg_dec_process(decoder, &io) == JPEG_ERR_OK) {
                    RgbToLvRgb888(output, static_cast<uint32_t>(output_size));
                    view = {output,        static_cast<uint32_t>(output_size),       header.width,
                            header.height, static_cast<uint32_t>(header.width) * 3U, MICROPIXEL_PIXEL_FORMAT_RGB888};
                    succeeded = true;
                }
                if (!succeeded) {
                    std::free(output);
                }
            }
        }
    }
    (void)jpeg_dec_close(decoder);
    return succeeded;
}

bool DecodePng(const micropixel_bundle_asset_view_t& asset, device::BitmapView& view) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, asset.data, asset.size)) {
        return false;
    }
    const bool has_alpha = (image.format & PNG_FORMAT_FLAG_ALPHA) != 0U;
    image.format = has_alpha ? PNG_FORMAT_BGRA : PNG_FORMAT_RGB;
    const uint32_t bytes_per_pixel = has_alpha ? 4U : 3U;
    size_t output_size = PNG_IMAGE_SIZE(image);
    if (!ValidDecodedSize(image.width, image.height, bytes_per_pixel, output_size)) {
        png_image_free(&image);
        return false;
    }
    auto* output =
        static_cast<uint8_t*>(heap_caps_aligned_alloc(64U, output_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (output == nullptr) {
        png_image_free(&image);
        return false;
    }
    bool succeeded = png_image_finish_read(&image, nullptr, output, 0, nullptr) != 0;
    if (succeeded) {
        if (!has_alpha) {
            RgbToLvRgb888(output, static_cast<uint32_t>(output_size));
        }
        view = {output,
                static_cast<uint32_t>(output_size),
                image.width,
                image.height,
                image.width * bytes_per_pixel,
                has_alpha ? MICROPIXEL_PIXEL_FORMAT_ARGB8888 : MICROPIXEL_PIXEL_FORMAT_RGB888};
    } else {
        std::free(output);
    }
    png_image_free(&image);
    return succeeded;
}

}  // namespace

DecodedBitmap::~DecodedBitmap() { std::free(const_cast<uint8_t*>(view_.data)); }

void DecodedBitmap::ReleaseOwnership() { view_.data = nullptr; }

bool DecodeBitmap(const micropixel_bundle_asset_view_t& asset, DecodedBitmap& decoded) {
    if (decoded.valid()) {
        return false;
    }
    if (asset.format == MICROPIXEL_BUNDLE_FORMAT_JPEG) {
        return DecodeJpeg(asset, decoded.view_);
    }
    if (asset.format == MICROPIXEL_BUNDLE_FORMAT_PNG) {
        return DecodePng(asset, decoded.view_);
    }
    return false;
}

}  // namespace micropixel::runtime
