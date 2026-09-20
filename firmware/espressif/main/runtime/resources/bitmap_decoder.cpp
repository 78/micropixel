#include "runtime/resources/bitmap_decoder.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "png.h"
#include "runtime/bundle/bundle_section_reader.hpp"
#include "runtime/wamr/diagnostics.h"
#include "sdkconfig.h"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "micropixel_bitmap";
constexpr uint32_t kBitmapPsramCapabilities = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#if CONFIG_IDF_TARGET_ESP32P4
constexpr uint64_t kBitmapAlignment = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
#else
constexpr uint64_t kBitmapAlignment = 128U;
#endif

void ReadPngBytes(png_structp png, png_bytep output, png_size_t size) {
    auto* reader = static_cast<BundleSectionReader*>(png_get_io_ptr(png));
    if (reader == nullptr) {
        png_error(png, "PNG reader missing");
        return;
    }
    if (!reader->Read({output, size})) png_error(png, reader->FailureDetail());
}

// The error buffer belongs to the caller and survives libpng's longjmp.
void PngError(png_structp png, png_const_charp message) {
    auto* detail = static_cast<char*>(png_get_error_ptr(png));
    std::strncpy(detail, message != nullptr ? message : "PNG decode failed", 95U);
    detail[95U] = '\0';
    png_longjmp(png, 1);
}

void PngWarning(png_structp, png_const_charp) {}

png_voidp PngPsramAlloc(png_structp, png_alloc_size_t size) { return heap_caps_malloc(size, kBitmapPsramCapabilities); }

void PngPsramFree(png_structp, png_voidp memory) { heap_caps_free(memory); }

bool ValidDecodedSize(uint32_t width, uint32_t height, uint32_t bytes_per_pixel, uint64_t size) {
    const uint64_t expected_size = static_cast<uint64_t>(width) * height * bytes_per_pixel;
    return width > 0U && height > 0U && width <= UINT16_MAX && height <= UINT16_MAX &&
           (bytes_per_pixel == 2U || bytes_per_pixel == 3U || bytes_per_pixel == 4U) && size == expected_size &&
           static_cast<uint64_t>(width) * bytes_per_pixel <= UINT16_MAX && expected_size <= UINT32_MAX;
}

bool BitmapLayout(uint32_t width, uint32_t height, uint32_t bytes_per_pixel, uint32_t alignment_pixels,
                  uint32_t& stride, uint32_t& size) {
    if (width == 0U || height == 0U || width > UINT16_MAX || height > UINT16_MAX || bytes_per_pixel < 2U ||
        bytes_per_pixel > 4U || alignment_pixels == 0U || (alignment_pixels & (alignment_pixels - 1U)) != 0U ||
        width > UINT32_MAX - (alignment_pixels - 1U)) {
        return false;
    }
    const uint32_t storage_width = (width + alignment_pixels - 1U) & ~(alignment_pixels - 1U);
    const uint64_t row_bytes = static_cast<uint64_t>(storage_width) * bytes_per_pixel;
    const uint64_t pixel_bytes = row_bytes * height;
    const uint64_t allocation_bytes =
        alignment_pixels == 1U ? pixel_bytes : (pixel_bytes + kBitmapAlignment - 1U) & ~(kBitmapAlignment - 1U);
    if (row_bytes > UINT16_MAX || allocation_bytes > UINT32_MAX) {
        return false;
    }
    stride = static_cast<uint32_t>(row_bytes);
    size = static_cast<uint32_t>(allocation_bytes);
    return true;
}

// A monotonic map permits a single source row to serve every destination row.
// Preserve authored atlas borders, including the final row/column, without blending alpha.
uint32_t MapPngSample(uint32_t index, uint32_t destination_extent, uint32_t source_extent) {
    if (destination_extent <= 1U || source_extent <= 1U) {
        return 0U;
    }
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(index) * (source_extent - 1U) + (destination_extent - 1U) / 2U) /
        (destination_extent - 1U));
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
    const uint64_t minimum_output_size = static_cast<uint64_t>(width) * height * 3U;
    return ValidDecodedSize(width, height, 3U, minimum_output_size);
}

bool DecodeJpeg(const micropixel_bundle_asset_view_t& asset, uint32_t preferred_opaque_format,
                device::BitmapView& view) {
    const bool rgb565 = preferred_opaque_format == MICROPIXEL_PIXEL_FORMAT_RGB565;
    const uint32_t bytes_per_pixel = rgb565 ? 2U : 3U;
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = rgb565 ? JPEG_PIXEL_FORMAT_RGB565_LE : JPEG_PIXEL_FORMAT_RGB888;
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
        const uint64_t output_size_64 = static_cast<uint64_t>(header.width) * header.height * bytes_per_pixel;
        if (ValidDecodedSize(header.width, header.height, bytes_per_pixel, output_size_64)) {
            const size_t output_size = static_cast<size_t>(output_size_64);
            auto* output = static_cast<uint8_t*>(heap_caps_aligned_alloc(64U, output_size, kBitmapPsramCapabilities));
            if (output != nullptr) {
                io.outbuf = output;
                if (jpeg_dec_process(decoder, &io) == JPEG_ERR_OK) {
                    if (!rgb565) {
                        RgbToLvRgb888(output, static_cast<uint32_t>(output_size));
                    }
                    view = {output,
                            static_cast<uint32_t>(output_size),
                            header.width,
                            header.height,
                            static_cast<uint32_t>(header.width) * bytes_per_pixel,
                            rgb565 ? MICROPIXEL_PIXEL_FORMAT_RGB565 : MICROPIXEL_PIXEL_FORMAT_BGR888};
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

void PackRgb565Row(const uint8_t* rgb, uint8_t* output, uint32_t width) {
    for (uint32_t x = 0U; x < width; ++x) {
        const uint16_t packed = static_cast<uint16_t>(((rgb[x * 3U] >> 3U) << 11U) | ((rgb[x * 3U + 1U] >> 2U) << 5U) |
                                                      (rgb[x * 3U + 2U] >> 3U));
        output[x * 2U] = static_cast<uint8_t>(packed);
        output[x * 2U + 1U] = static_cast<uint8_t>(packed >> 8U);
    }
}

bool DecodePng(BundleSectionReader& reader, const PngDecodeOptions& options, device::BitmapView& view,
               char* failure_detail) {
    micropixel_check_heap("before PNG decode");
    uint32_t expected_width = 0U;
    uint32_t expected_height = 0U;
    uint8_t header[24];
    if (!reader.PeekPrefix(header)) {
        (void)std::snprintf(failure_detail, 96U, "%s", reader.FailureDetail());
        return false;
    }
    micropixel_bundle_asset_view_t prefix{};
    prefix.data = header;
    prefix.size = sizeof(header);
    if (!PreflightPng(prefix, expected_width, expected_height)) {
        std::strcpy(failure_detail, "PNG dimensions or header invalid");
        ESP_LOGE(kTag, "PNG rejected before decode: bytes=%u", reader.size());
        return false;
    }

    png_structp png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, failure_detail, PngError, PngWarning, nullptr,
                                               PngPsramAlloc, PngPsramFree);
    if (png == nullptr) {
        std::strcpy(failure_detail, "PNG decoder allocation failed");
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        std::strcpy(failure_detail, "PNG info allocation failed");
        png_destroy_read_struct(&png, nullptr, nullptr);
        return false;
    }

    // The pointers themselves must survive longjmp; volatile pointees are insufficient.
    // Do not construct RAII owners between setjmp and any libpng call that can jump.
    uint8_t* volatile decoded = nullptr;
    uint8_t* volatile row_buffer = nullptr;
    if (setjmp(png_jmpbuf(png)) != 0) {
        heap_caps_free(row_buffer);
        heap_caps_free(decoded);
        png_destroy_read_struct(&png, &info, nullptr);
        ESP_LOGE(kTag, "streaming libpng decode failed: bytes=%u reason=%s free-psram=%u largest=%u", reader.size(),
                 failure_detail, static_cast<unsigned>(heap_caps_get_free_size(kBitmapPsramCapabilities)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(kBitmapPsramCapabilities)));
        return false;
    }

    png_set_read_fn(png, &reader, ReadPngBytes);
    png_set_user_limits(png, UINT16_MAX, UINT16_MAX);
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
    const bool has_transparency_chunk = png_get_valid(png, info, PNG_INFO_tRNS) != 0U;
    const bool has_alpha = (color_type & PNG_COLOR_MASK_ALPHA) != 0 || has_transparency_chunk;
    if (has_transparency_chunk) {
        png_set_tRNS_to_alpha(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    const bool rgb565 = !has_alpha && options.preferred_opaque_format == MICROPIXEL_PIXEL_FORMAT_RGB565;
    // LVGL's RGB888/ARGB8888 byte layouts on this little-endian target are
    // BGR/BGRA. RGB565 uses an explicit little-endian row pack below.
    if (!rgb565) {
        png_set_bgr(png);
    }
    png_read_update_info(png, info);

    const uint32_t decoded_bytes_per_pixel = has_alpha ? 4U : 3U;
    const uint32_t bytes_per_pixel = rgb565 ? 2U : decoded_bytes_per_pixel;
    const uint32_t pixel_format = has_alpha
                                      ? MICROPIXEL_PIXEL_FORMAT_BGRA8888
                                      : (rgb565 ? MICROPIXEL_PIXEL_FORMAT_RGB565 : MICROPIXEL_PIXEL_FORMAT_BGR888);
    const uint64_t output_size_64 = static_cast<uint64_t>(width) * height * bytes_per_pixel;
    if (!ValidDecodedSize(width, height, bytes_per_pixel, output_size_64) ||
        png_get_channels(png, info) != decoded_bytes_per_pixel ||
        png_get_rowbytes(png, info) != static_cast<size_t>(width) * decoded_bytes_per_pixel) {
        png_error(png, "unsupported PNG bitmap pixel layout");
    }
    const auto scaled_dimension = [&options](uint32_t dimension) {
        return static_cast<uint32_t>(
            (static_cast<uint64_t>(dimension) * options.scale_numerator + options.scale_denominator / 2U) /
            options.scale_denominator);
    };
    const uint32_t output_width = scaled_dimension(width);
    const uint32_t output_height = scaled_dimension(height);
    uint32_t output_stride = 0U;
    uint32_t output_size = 0U;
    if (!BitmapLayout(output_width, output_height, bytes_per_pixel, options.stride_alignment_pixels, output_stride,
                      output_size)) {
        png_error(png, "PNG scaled dimensions or stride invalid");
    }
    decoded = static_cast<uint8_t*>(heap_caps_aligned_alloc(kBitmapAlignment, output_size, kBitmapPsramCapabilities));
    if (decoded == nullptr) {
        char detail[96];
        (void)std::snprintf(detail, sizeof(detail),
                            "PNG output allocation failed: %" PRIu32 "x%" PRIu32 " stride=%" PRIu32 " bytes=%" PRIu32,
                            output_width, output_height, output_stride, output_size);
        png_error(png, detail);
    }

    const bool resample = output_width != width || output_height != height;
    if (rgb565 || resample) {
        row_buffer = static_cast<uint8_t*>(
            heap_caps_malloc(static_cast<size_t>(width) * decoded_bytes_per_pixel, kBitmapPsramCapabilities));
        if (row_buffer == nullptr) {
            png_error(png, "PNG source row allocation failed");
        }
    }
    uint32_t destination_y = 0U;
    for (uint32_t row = 0U; row < height; ++row) {
        if (row_buffer == nullptr) {
            uint8_t* destination = decoded + static_cast<size_t>(row) * output_stride;
            png_read_row(png, destination, nullptr);
            continue;
        }
        // Even unselected rows must pass through PNG's filters and CRC validation.
        png_read_row(png, row_buffer, nullptr);
        while (destination_y < output_height && MapPngSample(destination_y, output_height, height) == row) {
            uint8_t* destination = decoded + static_cast<size_t>(destination_y) * output_stride;
            if (output_width == width) {
                if (rgb565) {
                    PackRgb565Row(row_buffer, destination, width);
                } else {
                    std::memcpy(destination, row_buffer, static_cast<size_t>(width) * bytes_per_pixel);
                }
            } else {
                for (uint32_t x = 0U; x < output_width; ++x) {
                    const uint32_t source_x = MapPngSample(x, output_width, width);
                    const uint8_t* pixel = row_buffer + static_cast<size_t>(source_x) * decoded_bytes_per_pixel;
                    uint8_t* output_pixel = destination + static_cast<size_t>(x) * bytes_per_pixel;
                    if (rgb565) {
                        PackRgb565Row(pixel, output_pixel, 1U);
                    } else {
                        std::memcpy(output_pixel, pixel, bytes_per_pixel);
                    }
                }
            }
            ++destination_y;
        }
    }
    heap_caps_free(row_buffer);
    row_buffer = nullptr;
    png_read_end(png, info);
    if (!reader.Finish()) png_error(png, reader.FailureDetail());
    png_destroy_read_struct(&png, &info, nullptr);

    micropixel_check_heap("after PNG decode");
    view = {decoded, output_size, output_width, output_height, output_stride, pixel_format};
    ESP_LOGD(kTag,
             "streaming libpng decoded: %" PRIu32 "x%" PRIu32 " -> %" PRIu32 "x%" PRIu32 " bytes=%" PRIu32
             " format=%" PRIu32,
             width, height, output_width, output_height, output_size, pixel_format);
    return true;
}

}  // namespace

DecodedBitmap::~DecodedBitmap() { heap_caps_free(const_cast<uint8_t*>(view_.data)); }

void DecodedBitmap::ReleaseOwnership() { view_.data = nullptr; }

bool AllocateBitmap(uint32_t width, uint32_t height, uint32_t pixel_format, DecodedBitmap& bitmap,
                    uint32_t stride_alignment_pixels) {
    if (bitmap.valid()) {
        return false;
    }
    const uint32_t bytes_per_pixel = pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888
                                         ? 3U
                                         : (pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888
                                                ? 4U
                                                : (pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB565 ? 2U : 0U));
    uint32_t stride = 0U;
    uint32_t allocation_bytes = 0U;
    if (!BitmapLayout(width, height, bytes_per_pixel, stride_alignment_pixels, stride, allocation_bytes)) {
        return false;
    }
    auto* pixels = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(kBitmapAlignment, static_cast<size_t>(allocation_bytes), kBitmapPsramCapabilities));
    if (pixels == nullptr) {
        return false;
    }
    bitmap.view_ = {pixels, static_cast<uint32_t>(allocation_bytes), width,
                    height, static_cast<uint32_t>(stride),           pixel_format};
    return true;
}

bool DecodeBitmap(const micropixel_bundle_asset_view_t& asset, DecodedBitmap& decoded) {
    return DecodeBitmap(asset, MICROPIXEL_PIXEL_FORMAT_BGR888, decoded);
}

bool DecodeBitmap(const micropixel_bundle_asset_view_t& asset, uint32_t preferred_opaque_format,
                  DecodedBitmap& decoded) {
    std::strcpy(decoded.failure_detail_.data(), "Bitmap decode failed");
    if (decoded.valid()) {
        return false;
    }
    if (preferred_opaque_format != MICROPIXEL_PIXEL_FORMAT_BGR888 &&
        preferred_opaque_format != MICROPIXEL_PIXEL_FORMAT_RGB565) {
        return false;
    }
    if (asset.format == MICROPIXEL_BUNDLE_FORMAT_JPEG) {
        return DecodeJpeg(asset, preferred_opaque_format, decoded.view_);
    }
    if (asset.format == MICROPIXEL_BUNDLE_FORMAT_PNG) {
        return DecodePngBitmap(asset, PngDecodeOptions{preferred_opaque_format}, decoded);
    }
    return false;
}

bool DecodePngBitmap(const micropixel_bundle_asset_view_t& asset, const PngDecodeOptions& options,
                     DecodedBitmap& decoded) {
    if (decoded.valid()) return false;
    BundleSectionReader reader;
    if (asset.format != MICROPIXEL_BUNDLE_FORMAT_PNG || asset.data == nullptr ||
        !reader.OpenMemory({asset.data, asset.size})) {
        std::strcpy(decoded.failure_detail_.data(), "PNG input invalid");
        return false;
    }
    return DecodePngBitmap(reader, options, decoded);
}

bool DecodePngBitmap(BundleSectionReader& reader, const PngDecodeOptions& options, DecodedBitmap& decoded) {
    if (decoded.valid()) {
        return false;
    }
    std::strcpy(decoded.failure_detail_.data(), "PNG decode options invalid");
    if (options.scale_numerator == 0U || options.scale_denominator == 0U || options.scale_numerator > 4096U ||
        options.scale_denominator > 4096U ||
        (options.preferred_opaque_format != MICROPIXEL_PIXEL_FORMAT_BGR888 &&
         options.preferred_opaque_format != MICROPIXEL_PIXEL_FORMAT_RGB565)) {
        return false;
    }
    return DecodePng(reader, options, decoded.view_, decoded.failure_detail_.data());
}

}  // namespace micropixel::runtime
