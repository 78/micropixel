// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unordered_map>
#include <vector>

#include "esp_heap_caps.h"
#include "runtime/bundle/bundle_section_reader.hpp"
#include "runtime/resources/bitmap_decoder.hpp"

namespace {
std::unordered_map<void*, size_t> allocations;
size_t used{}, peak{}, budget = 8U * 1024 * 1024;
std::vector<uint8_t> input;
bool source_size(const micropixel_bundle_source_t*, uint32_t* size) {
    *size = input.size();
    return true;
}
bool source_read(const micropixel_bundle_source_t*, uint32_t offset, void* dest, uint32_t bytes) {
    if (offset > input.size() || bytes > input.size() - offset) return false;
    std::memcpy(dest, input.data() + offset, bytes);
    return true;
}
const micropixel_bundle_source_ops_t ops{source_size, source_read, nullptr};
const micropixel_bundle_source_t source{&ops, {}};
uint32_t hash() {
    uint32_t h = 2166136261U;
    for (auto b : input) h = (h ^ b) * 16777619U;
    return h;
}
void read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    assert(f);
    input.assign(std::istreambuf_iterator<char>(f), {});
}
bool decode(micropixel::runtime::PngDecodeOptions options, bool bad_hash = false) {
    using namespace micropixel::runtime;
    assert(used == 0);
    peak = 0;
    {
        BundleSectionReader reader;
        micropixel_bundle_section_t section{};
        section.size = input.size();
        section.hash = hash() ^ (bad_hash ? 1 : 0);
        assert(reader.Open(source, section));
        DecodedBitmap bitmap;
        const bool ok = DecodePngBitmap(reader, options, bitmap);
        if (!ok) {
            assert(!bitmap.valid());
            return false;
        }
        const auto& v = bitmap.view();
        assert(v.width == 1024U * options.scale_numerator / options.scale_denominator);
        assert(v.height == 1632U * options.scale_numerator / options.scale_denominator);
        assert(v.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888);
        for (auto x : {0U, v.width / 2, v.width - 1})
            for (auto y : {0U, v.height / 2, v.height - 1}) {
                uint32_t sx = v.width > 1 ? ((uint64_t)x * 1023 + (v.width - 1) / 2) / (v.width - 1) : 0;
                uint32_t sy = v.height > 1 ? ((uint64_t)y * 1631 + (v.height - 1) / 2) / (v.height - 1) : 0;
                auto* p = v.data + (size_t)y * v.stride + x * 4;
                assert(p[0] == (sx + sy) % 256 && p[1] == sy % 256 && p[2] == sx % 256 && p[3] == sx % 255);
            }
        assert(peak < v.size + 256U * 1024U);  // Final pixels plus bounded rows/inflater.
    }
    assert(used == 0);
    return true;
}
}  // namespace
extern "C" {
void* heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t) {
    if (size > budget - used) return nullptr;
    void* p = nullptr;
    if (posix_memalign(&p, std::max(alignment, sizeof(void*)), std::max(size, size_t(1)))) return nullptr;
    allocations[p] = size;
    used += size;
    peak = std::max(peak, used);
    return p;
}
void* heap_caps_malloc(size_t size, uint32_t caps) { return heap_caps_aligned_alloc(8, size, caps); }
void heap_caps_free(void* p) {
    if (!p) return;
    used -= allocations.at(p);
    allocations.erase(p);
    std::free(p);
}
void* heap_caps_realloc(void* p, size_t size, uint32_t caps) {
    auto* n = heap_caps_malloc(size, caps);
    if (n && p) {
        std::memcpy(n, p, std::min(size, allocations.at(p)));
        heap_caps_free(p);
    }
    return n;
}
size_t heap_caps_get_free_size(uint32_t) { return budget - used; }
size_t heap_caps_get_largest_free_block(uint32_t) { return budget - used; }
}
int main(int argc, char** argv) {
    using namespace micropixel::runtime;
    assert(argc == 3);
    read_file(argv[1]);
    assert(decode({MICROPIXEL_PIXEL_FORMAT_BGR888, 1, 2, 32}));
    assert(decode({MICROPIXEL_PIXEL_FORMAT_BGR888, 1, 1, 1}));
    assert(!decode({MICROPIXEL_PIXEL_FORMAT_BGR888, 2, 1, 1}));
    assert(used == 0);
    assert(!decode({MICROPIXEL_PIXEL_FORMAT_BGR888, 1, 1, 3}));
    assert(used == 0);
    assert(!decode({MICROPIXEL_PIXEL_FORMAT_BGR888, 1, 1, 1}, true));
    assert(used == 0);
    input.resize(input.size() - 30);
    assert(!decode({MICROPIXEL_PIXEL_FORMAT_BGR888, 1, 1, 1}));
    assert(used == 0);
    read_file(argv[1]);
    budget = 65536;
    assert(!decode({MICROPIXEL_PIXEL_FORMAT_BGR888, 1, 1, 1}));
    assert(used == 0);
    budget = 8U * 1024 * 1024;
    read_file(argv[2]);
    {
        micropixel_bundle_asset_view_t a{};
        a.data = input.data();
        a.size = input.size();
        a.format = MICROPIXEL_BUNDLE_FORMAT_PNG;
        DecodedBitmap b;
        assert(DecodePngBitmap(a, {MICROPIXEL_PIXEL_FORMAT_RGB565, 1, 1, 8}, b));
        assert(b.view().width == 3 && b.view().height == 2 && b.view().stride == 16);
        assert(b.view().pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB565);
        assert(b.view().data[0] == 0 && b.view().data[1] == 0xf8);  // Red, little endian RGB565.
    }
    assert(used == 0);
    std::cout << "PASS: large PNG, bounded peak memory, sampling, RGB565, hash/truncation and allocation failure\n";
}
