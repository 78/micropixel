// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

#include "esp_jpeg_dec.h"
#include "runtime/bundle/bundle_section_reader.hpp"
#include "runtime/resources/bitmap_decoder.hpp"
#include "zlib.h"

namespace {
using micropixel::runtime::DecodedBitmap;
using micropixel::runtime::DecodePngBitmap;
using micropixel::runtime::PngDecodeOptions;

std::unordered_map<void*, size_t> allocations;
size_t live_bytes{}, peak_bytes{}, largest_request{}, allocation_calls{}, fail_at{};
size_t allocation_limit = std::numeric_limits<size_t>::max();

void ResetHeap() {
    assert(allocations.empty());
    live_bytes = peak_bytes = largest_request = allocation_calls = fail_at = 0;
    allocation_limit = std::numeric_limits<size_t>::max();
}

void Append32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<uint8_t>(value >> shift));
}

void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& payload) {
    Append32(png, static_cast<uint32_t>(payload.size()));
    const size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), payload.begin(), payload.end());
    Append32(png, static_cast<uint32_t>(crc32(0, png.data() + start, png.size() - start)));
}

// Fixtures exercise the actual pinned libpng reader, including filters, CRC and inflate.
std::vector<uint8_t> MakePng(uint32_t width, uint32_t height, uint8_t color, uint8_t depth,
                             const std::vector<uint8_t>& pixels, const std::vector<uint8_t>& palette = {},
                             const std::vector<uint8_t>& transparency = {}, uint8_t interlace = 0) {
    std::vector<uint8_t> png{137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> header;
    Append32(header, width);
    Append32(header, height);
    header.insert(header.end(), {depth, color, 0, 0, interlace});
    Chunk(png, "IHDR", header);
    if (!palette.empty()) Chunk(png, "PLTE", palette);
    if (!transparency.empty()) Chunk(png, "tRNS", transparency);
    assert(height && pixels.size() % height == 0);
    const size_t row_bytes = pixels.size() / height;
    std::vector<uint8_t> filtered;
    for (uint32_t y = 0; y < height; ++y) {
        // Up filter ensures even discarded rows must be decoded.
        filtered.push_back(2);
        for (size_t x = 0; x < row_bytes; ++x) {
            filtered.push_back(pixels[y * row_bytes + x] - (y ? pixels[(y - 1) * row_bytes + x] : 0));
        }
    }
    uLongf size = compressBound(filtered.size());
    std::vector<uint8_t> compressed(size);
    assert(compress2(compressed.data(), &size, filtered.data(), filtered.size(), Z_BEST_SPEED) == Z_OK);
    compressed.resize(size);
    Chunk(png, "IDAT", compressed);
    Chunk(png, "IEND", {});
    return png;
}

micropixel_bundle_asset_view_t Asset(const std::vector<uint8_t>& png) {
    micropixel_bundle_asset_view_t asset{};
    asset.data = png.data();
    asset.size = static_cast<uint32_t>(png.size());
    asset.format = MICROPIXEL_BUNDLE_FORMAT_PNG;
    return asset;
}

std::vector<uint8_t> RgbaPixels(uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            pixels.insert(pixels.end(), {static_cast<uint8_t>(x), static_cast<uint8_t>(y), 90,
                                         static_cast<uint8_t>((x + y) % 3 * 127)});
        }
    }
    return pixels;
}

void CheckPixel(const DecodedBitmap& bitmap, uint32_t x, uint32_t y, uint32_t source_x, uint32_t source_y) {
    const auto* pixel = bitmap.view().data + y * bitmap.view().stride + x * 4;
    assert(pixel[0] == 90 && pixel[1] == source_y && pixel[2] == source_x);
    assert(pixel[3] == (source_x + source_y) % 3 * 127);
}

void SamplingAndFormats() {
    const auto png = MakePng(5, 5, 6, 8, RgbaPixels(5, 5));
    {
        DecodedBitmap bitmap;
        assert(DecodePngBitmap(Asset(png), {MICROPIXEL_PIXEL_FORMAT_RGB565, 2, 3, 32}, bitmap));
        assert(bitmap.view().width == 3 && bitmap.view().height == 3);
        assert(bitmap.view().stride == 128 && bitmap.view().size == 384);
        assert(bitmap.view().pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888);
        for (uint32_t y = 0; y < 3; ++y)
            for (uint32_t x = 0; x < 3; ++x) CheckPixel(bitmap, x, y, x * 2, y * 2);
    }
    {
        DecodedBitmap bitmap;
        assert(micropixel::runtime::DecodeBitmap(Asset(png), bitmap));
        for (uint32_t y = 0; y < 5; ++y)
            for (uint32_t x = 0; x < 5; ++x) CheckPixel(bitmap, x, y, x, y);
        const auto* retained = bitmap.view().data;
        assert(!DecodePngBitmap(Asset(png), {}, bitmap));
        assert(bitmap.view().data == retained);
    }
    {
        DecodedBitmap bitmap;
        assert(DecodePngBitmap(Asset(png), {MICROPIXEL_PIXEL_FORMAT_BGR888, 1, 5}, bitmap));
        assert(bitmap.view().width == 1 && bitmap.view().height == 1);
        CheckPixel(bitmap, 0, 0, 0, 0);
    }
    {
        const auto small = MakePng(3, 3, 6, 8, RgbaPixels(3, 3));
        DecodedBitmap bitmap;
        assert(DecodePngBitmap(Asset(small), {MICROPIXEL_PIXEL_FORMAT_BGR888, 5, 3}, bitmap));
        constexpr std::array<uint32_t, 5> expected{0, 1, 1, 2, 2};
        for (uint32_t y = 0; y < 5; ++y)
            for (uint32_t x = 0; x < 5; ++x) CheckPixel(bitmap, x, y, expected[x], expected[y]);
    }
    {
        const auto rgb = MakePng(3, 1, 2, 8, {255, 128, 32, 0, 255, 0, 0, 0, 255});
        DecodedBitmap packed;
        assert(DecodePngBitmap(Asset(rgb), {MICROPIXEL_PIXEL_FORMAT_RGB565, 2, 3, 32}, packed));
        assert(packed.view().width == 2 && packed.view().height == 1 && packed.view().stride == 64);
        assert(packed.view().pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB565);
        const uint8_t expected[]{0x04, 0xfc, 0x1f, 0x00};
        assert(std::memcmp(packed.view().data, expected, sizeof(expected)) == 0);
        DecodedBitmap bgr;
        assert(DecodePngBitmap(Asset(rgb), {MICROPIXEL_PIXEL_FORMAT_BGR888, 2, 3}, bgr));
        const uint8_t expected_bgr[]{32, 128, 255, 255, 0, 0};
        assert(bgr.view().pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888);
        assert(std::memcmp(bgr.view().data, expected_bgr, sizeof(expected_bgr)) == 0);
    }
    {
        const auto indexed = MakePng(3, 1, 3, 8, {0, 1, 2}, {255, 0, 0, 0, 255, 0, 0, 0, 255}, {0, 127, 255});
        DecodedBitmap bitmap;
        assert(DecodePngBitmap(Asset(indexed), {MICROPIXEL_PIXEL_FORMAT_RGB565}, bitmap));
        const uint8_t expected[]{0, 0, 255, 0, 0, 255, 0, 127, 255, 0, 0, 255};
        assert(bitmap.view().pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888);
        assert(std::memcmp(bitmap.view().data, expected, sizeof(expected)) == 0);
    }
    {
        const auto gray = MakePng(2, 1, 0, 16, {0x12, 0x34, 0xab, 0xcd});
        DecodedBitmap bitmap;
        assert(DecodePngBitmap(Asset(gray), {}, bitmap));
        const uint8_t expected[]{0x12, 0x12, 0x12, 0xab, 0xab, 0xab};
        assert(std::memcmp(bitmap.view().data, expected, sizeof(expected)) == 0);
    }
    assert(allocations.empty());
}

void BoundedMemoryAndFailures() {
    ResetHeap();
    const auto large = MakePng(1024, 1024, 6, 8, RgbaPixels(1024, 1024));
    allocation_limit = 2 * 1024 * 1024;
    {
        DecodedBitmap bitmap;
        assert(DecodePngBitmap(Asset(large), {MICROPIXEL_PIXEL_FORMAT_RGB565, 2, 3, 32}, bitmap));
        assert(bitmap.view().width == 683 && bitmap.view().height == 683);
        assert(bitmap.view().size == 704 * 683 * 4);
        assert(largest_request == bitmap.view().size);
        assert(peak_bytes < bitmap.view().size + 128 * 1024);
        assert(live_bytes == bitmap.view().size);
    }
    assert(allocations.empty());
    ResetHeap();
    const auto png = MakePng(32, 32, 6, 8, RgbaPixels(32, 32));
    const PngDecodeOptions options{MICROPIXEL_PIXEL_FORMAT_BGR888, 2, 3, 32};
    {
        DecodedBitmap bitmap;
        assert(DecodePngBitmap(Asset(png), options, bitmap));
    }
    const size_t calls = allocation_calls;
    bool saw_output_failure = false;
    bool saw_row_failure = false;
    for (size_t failure = 1; failure <= calls; ++failure) {
        ResetHeap();
        fail_at = failure;
        {
            DecodedBitmap bitmap;
            const bool result = DecodePngBitmap(Asset(png), options, bitmap);
            assert(result == bitmap.valid());
            if (!result) {
                assert(bitmap.FailureDetail()[0]);
                if (std::strstr(bitmap.FailureDetail(), "PNG output allocation failed")) {
                    saw_output_failure = true;
                    assert(std::strstr(bitmap.FailureDetail(), "21x21 stride=128 bytes=2688"));
                }
                if (std::strstr(bitmap.FailureDetail(), "PNG source row allocation failed")) saw_row_failure = true;
            }
        }
        assert(allocations.empty() && live_bytes == 0);
    }
    assert(saw_output_failure && saw_row_failure);
    ResetHeap();
    for (size_t cut : {size_t{20}, png.size() / 2, png.size() - 8}) {
        auto truncated = png;
        truncated.resize(cut);
        DecodedBitmap bitmap;
        assert(!DecodePngBitmap(Asset(truncated), options, bitmap));
        assert(!bitmap.valid() && allocations.empty());
    }
    {
        auto corrupted = png;
        corrupted[corrupted.size() - 13] ^= 1;  // IDAT CRC; IEND CRC is only a warning in libpng.
        DecodedBitmap bitmap;
        assert(!DecodePngBitmap(Asset(corrupted), options, bitmap));
        assert(!bitmap.valid() && allocations.empty());
    }
    {
        const auto interlaced = MakePng(1, 1, 6, 8, {1, 2, 3, 4}, {}, {}, 1);
        DecodedBitmap bitmap;
        assert(!DecodePngBitmap(Asset(interlaced), {}, bitmap));
        assert(allocations.empty());
    }
    for (const PngDecodeOptions invalid :
         {PngDecodeOptions{MICROPIXEL_PIXEL_FORMAT_BGR888, 0, 1},
          PngDecodeOptions{MICROPIXEL_PIXEL_FORMAT_BGR888, 1, 0},
          PngDecodeOptions{MICROPIXEL_PIXEL_FORMAT_BGR888, 4097, 1},
          PngDecodeOptions{MICROPIXEL_PIXEL_FORMAT_BGR888, 1, 4096},  // Rounded zero.
          PngDecodeOptions{MICROPIXEL_PIXEL_FORMAT_BGR888, 4096, 1},  // Oversized output.
          PngDecodeOptions{MICROPIXEL_PIXEL_FORMAT_BGR888, 1, 1, 3},
          PngDecodeOptions{MICROPIXEL_PIXEL_FORMAT_BGRA8888}}) {
        DecodedBitmap bitmap;
        assert(!DecodePngBitmap(Asset(png), invalid, bitmap));
        assert(!bitmap.valid() && allocations.empty());
    }
}

uint32_t Hash(const std::vector<uint8_t>& bytes) {
    uint32_t hash = 2166136261U;
    for (uint8_t byte : bytes) hash = (hash ^ byte) * 16777619U;
    return hash;
}

struct CountingSource {
    std::vector<uint8_t> bytes;
    uint32_t next{64};
    uint32_t calls{};
    uint32_t fail_call{};
    static CountingSource& State(const micropixel_bundle_source_t* source) {
        CountingSource* state;
        std::memcpy(&state, source->state, sizeof(state));
        return *state;
    }
    static bool Size(const micropixel_bundle_source_t* source, uint32_t* size) {
        *size = static_cast<uint32_t>(State(source).bytes.size());
        return true;
    }
    static bool Read(const micropixel_bundle_source_t* source, uint32_t offset, void* output, uint32_t size) {
        auto& state = State(source);
        assert(offset == state.next && size <= 4096 && size > 0);
        assert(offset <= state.bytes.size() && size <= state.bytes.size() - offset);
        if (++state.calls == state.fail_call) return false;
        std::memcpy(output, state.bytes.data() + offset, size);
        state.next += size;
        return true;
    }
    micropixel_bundle_source_t Source() {
        static constexpr micropixel_bundle_source_ops_t ops{Size, Read, nullptr};
        micropixel_bundle_source_t result{};
        result.ops = &ops;
        auto* self = this;
        std::memcpy(result.state, &self, sizeof(self));
        return result;
    }
};

void LargeStreamingInput() {
    using micropixel::runtime::BundleSectionReader;
    ResetHeap();
    std::vector<uint8_t> pixels(256 * 256 * 3);
    uint32_t random = 1;
    for (auto& byte : pixels) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        byte = static_cast<uint8_t>(random);
    }
    const auto png = MakePng(256, 256, 2, 8, pixels);
    assert(png.size() > 128 * 1024);
    CountingSource storage;
    storage.bytes.resize(64);
    storage.bytes.insert(storage.bytes.end(), png.begin(), png.end());
    const auto source = storage.Source();
    micropixel_bundle_section_t section{};
    section.offset = 64;
    section.size = static_cast<uint32_t>(png.size());
    section.hash = Hash(png);
    const PngDecodeOptions options{MICROPIXEL_PIXEL_FORMAT_BGR888, 1, 16};
    allocation_limit = 64 * 1024;  // a complete compressed copy would fail
    {
        BundleSectionReader reader;
        assert(reader.Open(source, section));
        DecodedBitmap streamed;
        assert(DecodePngBitmap(reader, options, streamed));
        assert(peak_bytes < 128 * 1024);
        assert(storage.calls == (png.size() + 4095) / 4096 && storage.next == storage.bytes.size());
        DecodedBitmap memory;
        assert(DecodePngBitmap(Asset(png), options, memory));
        assert(streamed.view().size == memory.view().size);
        assert(std::memcmp(streamed.view().data, memory.view().data, memory.view().size) == 0);
    }
    assert(allocations.empty());
    // Fail within IDAT after libpng has allocated output and inflate state.
    storage.next = 64;
    storage.calls = 0;
    storage.fail_call = 3;
    {
        BundleSectionReader reader;
        assert(reader.Open(source, section));
        DecodedBitmap bitmap;
        assert(!DecodePngBitmap(reader, options, bitmap) && !bitmap.valid());
        assert(std::strstr(bitmap.FailureDetail(), "IO"));
    }
    assert(allocations.empty());
    ResetHeap();
}

void StreamingInput() {
    using micropixel::runtime::BundleSectionReader;
    ResetHeap();
    auto png = MakePng(32, 32, 6, 8, RgbaPixels(32, 32));
    // IEND may precede the end of the section: all trailing bytes must be hashed.
    png.resize(png.size() + 12000, 0xa5);
    CountingSource storage;
    storage.bytes.resize(64, 0x77);
    storage.bytes.insert(storage.bytes.end(), png.begin(), png.end());
    const auto source = storage.Source();
    micropixel_bundle_section_t section{};
    section.offset = 64;
    section.size = static_cast<uint32_t>(png.size());
    section.hash = Hash(png);
    section.format = MICROPIXEL_BUNDLE_FORMAT_PNG;
    const size_t expected_calls = (png.size() + 4095) / 4096;
    {
        BundleSectionReader reader;
        assert(reader.Open(source, section));
        DecodedBitmap bitmap;
        assert(DecodePngBitmap(reader, {}, bitmap));
        CheckPixel(bitmap, 31, 31, 31, 31);
        assert(storage.next == storage.bytes.size() && storage.calls == expected_calls);
        assert(reader.Finish());
        assert(storage.calls == expected_calls);
    }
    assert(allocations.empty());
    const size_t total_allocations = allocation_calls;
    for (size_t failure = 1; failure <= total_allocations; ++failure) {
        ResetHeap();
        fail_at = failure;
        storage.next = 64;
        storage.calls = 0;
        {
            BundleSectionReader reader;
            if (reader.Open(source, section)) {
                DecodedBitmap bitmap;
                const bool decoded = DecodePngBitmap(reader, {}, bitmap);
                assert(decoded == bitmap.valid());
            }
        }
        assert(allocations.empty());
    }
    ResetHeap();
    // IO failure while decoding or draining the section must discard the private bitmap.
    for (uint32_t failed_read = 1; failed_read <= expected_calls; ++failed_read) {
        storage.next = 64;
        storage.calls = 0;
        storage.fail_call = failed_read;
        {
            BundleSectionReader reader;
            assert(reader.Open(source, section));
            DecodedBitmap bitmap;
            assert(!DecodePngBitmap(reader, {}, bitmap) && !bitmap.valid());
            assert(std::strstr(bitmap.FailureDetail(), "IO"));
            assert(!reader.Finish());
            assert(storage.calls == failed_read);  // sticky failure: never retry into a different file version
        }
        assert(allocations.empty());
    }
    storage.fail_call = 0;
    storage.next = 64;
    storage.calls = 0;
    storage.bytes.back() ^= 1;
    {
        BundleSectionReader reader;
        assert(reader.Open(source, section));
        DecodedBitmap bitmap;
        assert(!DecodePngBitmap(reader, {}, bitmap) && !bitmap.valid());
        assert(std::strstr(bitmap.FailureDetail(), "hash"));
    }
    storage.bytes.back() ^= 1;
    assert(allocations.empty());
    // Mapped source shares exactly the same complete-section verification, with no IO or prefetch allocation.
    storage.calls = 0;
    {
        BundleSectionReader reader;
        const size_t before = allocation_calls;
        assert(reader.Open(source, section, storage.bytes));
        assert(allocation_calls == before);
        DecodedBitmap bitmap;
        assert(DecodePngBitmap(reader, {}, bitmap));
        assert(storage.calls == 0);
    }
    {
        BundleSectionReader reader;
        auto invalid = section;
        invalid.offset = UINT32_MAX - 1;
        assert(!reader.Open(source, invalid));
    }
    assert(allocations.empty());
    ResetHeap();
}

}  // namespace

void* micropixel_test_psram_allocate(size_t size) {
    ++allocation_calls;
    largest_request = std::max(largest_request, size);
    if (allocation_calls == fail_at || size > allocation_limit) return nullptr;
    void* memory = std::malloc(size);
    assert(memory);
    allocations.emplace(memory, size);
    live_bytes += size;
    peak_bytes = std::max(peak_bytes, live_bytes);
    return memory;
}

void micropixel_test_psram_free(void* memory) {
    if (!memory) return;
    const auto found = allocations.find(memory);
    assert(found != allocations.end());
    live_bytes -= found->second;
    allocations.erase(found);
    std::free(memory);
}

// JPEG is unchanged; use the vendor declarations and isolate its hardware library.
extern "C" jpeg_error_t jpeg_dec_open(jpeg_dec_config_t*, jpeg_dec_handle_t*) { return JPEG_ERR_FAIL; }
extern "C" jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t) { return JPEG_ERR_OK; }
extern "C" jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t, jpeg_dec_io_t*, jpeg_dec_header_info_t*) {
    return JPEG_ERR_FAIL;
}
extern "C" jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t, jpeg_dec_io_t*) { return JPEG_ERR_FAIL; }

int main() {
    SamplingAndFormats();
    BoundedMemoryAndFailures();
    StreamingInput();
    LargeStreamingInput();
    std::puts("bitmap_decoder tests passed");
}
