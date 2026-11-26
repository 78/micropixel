// SPDX-License-Identifier: Apache-2.0
#include <cstring>
#include <mutex>

#include "esp_heap_caps.h"
#include "graphics.hpp"
#include "media_display.h"
#define STBIW_MALLOC(n) heap_caps_malloc(n, MALLOC_CAP_SPIRAM)
#define STBIW_REALLOC(p, n) heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM)
#define STBIW_FREE(p) heap_caps_free(p)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
namespace mpx_port {
namespace {
struct Output {
    uint8_t* data{};
    size_t size{}, capacity{};
    bool failed{};
};
void Write(void* ctx, void* data, int n) {
    auto& o = *(Output*)ctx;
    if (o.failed || n <= 0) return;
    if ((size_t)n > 1024 * 1024 - o.size) {
        o.failed = true;
        return;
    }
    size_t need = o.size + n;
    if (need > o.capacity) {
        size_t capacity = o.capacity ? o.capacity * 2 : 16384;
        if (capacity < need) capacity = need;
        auto* p = (uint8_t*)heap_caps_realloc(o.data, capacity, MALLOC_CAP_SPIRAM);
        if (!p) {
            o.failed = true;
            return;
        }
        o.data = p;
        o.capacity = capacity;
    }
    memcpy(o.data + o.size, data, n);
    o.size += n;
}
}  // namespace
bool Graphics::CaptureJpeg(uint8_t*& jpeg, size_t& size) {
    auto* rgb = (uint8_t*)heap_caps_malloc((size_t)Width() * Height() * 3, MALLOC_CAP_SPIRAM);
    if (!rgb) return false;
    {
        std::lock_guard lock(mutex_);
        lv_color_format_t f;
        auto* source = (const uint16_t*)media_display_get_scanout(&f);
        if (!source || f != LV_COLOR_FORMAT_RGB565) {
            heap_caps_free(rgb);
            return false;
        }
        for (size_t i = 0; i < (size_t)Width() * Height(); ++i) {
            uint16_t p = source[i];
            rgb[i * 3] = ((p >> 11) & 31) * 255 / 31;
            rgb[i * 3 + 1] = ((p >> 5) & 63) * 255 / 63;
            rgb[i * 3 + 2] = (p & 31) * 255 / 31;
        }
    }
    Output out;
    bool ok = stbi_write_jpg_to_func(Write, &out, Width(), Height(), 3, rgb, 75) != 0 && !out.failed;
    heap_caps_free(rgb);
    if (!ok) {
        heap_caps_free(out.data);
        return false;
    }
    jpeg = out.data;
    size = out.size;
    return true;
}
}  // namespace mpx_port
