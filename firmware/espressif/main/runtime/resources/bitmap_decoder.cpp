#include "runtime/resources/bitmap_decoder.hpp"

#include <cinttypes>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "png.h"
#include "sdkconfig.h"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "micropixel_bitmap";

struct PngMemoryReader final {
    const uint8_t* data{};
    size_t size{};
    size_t offset{};
};

void ReadPngBytes(png_structp png, png_bytep output, png_size_t size) {
    auto* reader = static_cast<PngMemoryReader*>(png_get_io_ptr(png));
    if (reader == nullptr || reader->offset > reader->size || size > reader->size - reader->offset) {
        png_error(png, "truncated PNG bitmap");
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

uint32_t ReadBigEndian32(const uint8_t* value) {
    return (static_cast<uint32_t>(value[0]) << 24U) | (static_cast<uint32_t>(value[1]) << 16U) |
           (static_cast<uint32_t>(value[2]) << 8U) | static_cast<uint32_t>(value[3]);
}

bool PreflightPng(const micropixel_bundle_asset_view_t& asset, uint32_t& width, uint32_t& height) {
    constexpr uint8_t kSignature[] = {0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    constexpr uint8_t kIhdr[] = {'I', 'H', 'D', 'R'};
    if (asset.data == nullptr || asset.size < 24U || std::memcmp(asset.data, kSignature, sizeof(kSignature)) != 0 ||
        std::memcmp(asset.data + 12U, kIhdr, sizeof(kIhdr)) != 0) {
        return false;
    }
    width = ReadBigEndian32(asset.data + 16U);
    height = ReadBigEndian32(asset.data + 20U);
    const size_t output_size = static_cast<size_t>(width) * height * 4U;
    return ValidDecodedSize(width, height, 4U, output_size);
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
                            header.height, static_cast<uint32_t>(header.width) * 3U, MICROPIXEL_PIXEL_FORMAT_BGR888};
                    succeeded = true;
                }
                if (!succeeded) {
                    heap_caps_free(output);
                }
            }
        }
    }
    (void)jpeg_dec_close(decoder);
    return succeeded;
}

bool DecodePng(const micropixel_bundle_asset_view_t& asset, device::BitmapView& view) {
    uint32_t expected_width = 0U;
    uint32_t expected_height = 0U;
    if (!PreflightPng(asset, expected_width, expected_height)) {
        ESP_LOGE(kTag, "PNG rejected before decode: bytes=%u", asset.size);
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

    volatile uint8_t* decoded = nullptr;
    if (setjmp(png_jmpbuf(png)) != 0) {
        heap_caps_free(const_cast<uint8_t*>(decoded));
        png_destroy_read_struct(&png, &info, nullptr);
        ESP_LOGE(kTag, "streaming libpng decode failed: bytes=%u", asset.size);
        return false;
    }

    PngMemoryReader reader{asset.data, asset.size, 0U};
    png_set_read_fn(png, &reader, ReadPngBytes);
    png_set_user_limits(png, 720U, 720U);
    png_read_info(png, info);

    const uint32_t width = png_get_image_width(png, info);
    const uint32_t height = png_get_image_height(png, info);
    int color_type = png_get_color_type(png, info);
    const int bit_depth = png_get_bit_depth(png, info);
    if (width != expected_width || height != expected_height ||
        png_get_interlace_type(png, info) != PNG_INTERLACE_NONE) {
        png_error(png, "unsupported PNG bitmap dimensions or interlace");
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
    const bool has_transparency = png_get_valid(png, info, PNG_INFO_tRNS) != 0U;
    if (has_transparency) {
        png_set_tRNS_to_alpha(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    if ((color_type & PNG_COLOR_MASK_ALPHA) == 0 && !has_transparency) {
        png_set_add_alpha(png, UINT8_MAX, PNG_FILLER_AFTER);
    }
    // LVGL's ARGB8888 byte layout on this little-endian target is BGRA.
    png_set_bgr(png);
    png_read_update_info(png, info);

    const size_t output_size = static_cast<size_t>(width) * height * 4U;
    if (!ValidDecodedSize(width, height, 4U, output_size) || png_get_channels(png, info) != 4U ||
        png_get_rowbytes(png, info) != static_cast<size_t>(width) * 4U) {
        png_error(png, "unsupported PNG bitmap pixel layout");
    }
    decoded = static_cast<uint8_t*>(heap_caps_aligned_alloc(64U, output_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (decoded == nullptr) {
        png_error(png, "PNG bitmap output allocation failed");
    }

    for (uint32_t row = 0U; row < height; ++row) {
        png_read_row(png, const_cast<uint8_t*>(decoded) + static_cast<size_t>(row) * width * 4U, nullptr);
    }
    png_read_end(png, info);
    png_destroy_read_struct(&png, &info, nullptr);

    auto* pixels = const_cast<uint8_t*>(decoded);
    view = {pixels, static_cast<uint32_t>(output_size), width, height, width * 4U, MICROPIXEL_PIXEL_FORMAT_BGRA8888};
    ESP_LOGI(kTag, "streaming libpng decoded: %" PRIu32 "x%" PRIu32 " bytes=%zu output=%p", width, height, output_size,
             pixels);
    return true;
}

}  // namespace

DecodedBitmap::~DecodedBitmap() { heap_caps_free(const_cast<uint8_t*>(view_.data)); }

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
