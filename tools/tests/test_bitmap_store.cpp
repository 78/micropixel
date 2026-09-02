#include <array>
#include <cassert>
#include <cstddef>

#include "abi/micropixel_abi.h"
#include "device/contracts/graphics.hpp"
#include "runtime/resources/bitmap_store.hpp"
#include "runtime/runtime_limits.hpp"

namespace {

micropixel::device::BitmapView MakeView(const uint8_t* pixels) {
    return micropixel::device::BitmapView{
        pixels, 12U, 2U, 2U, 6U, MICROPIXEL_PIXEL_FORMAT_BGR888, 0U,
    };
}

}  // namespace

int main() {
    using micropixel::runtime::BitmapStore;
    using micropixel::runtime::limits::kMaxBitmaps;

    BitmapStore store;
    assert(store.valid());

    std::array<uint8_t, 12U> pixels{};
    const auto view = MakeView(pixels.data());
    const micropixel_texture_handle_t first = store.Add(view, false);
    assert(first != 0U);

    micropixel::device::BitmapView resolved{};
    assert(store.Resolve(first, resolved));
    assert(resolved.data == pixels.data());
    assert(resolved.size == pixels.size());
    assert(resolved.width == 2U);
    assert(resolved.height == 2U);
    assert(resolved.stride == 6U);
    assert(resolved.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888);

    store.Release(first);
    assert(!store.Resolve(first, resolved));

    const micropixel_texture_handle_t reused = store.Add(view, false, true);
    assert(reused != 0U);
    assert(reused != first);
    assert(!store.Resolve(first, resolved));
    assert(store.ResolveMutable(reused, resolved));

    assert(store.RetainSceneReference(reused));
    store.Release(reused);
    assert(!store.Resolve(reused, resolved));
    store.ReleaseSceneReference(reused);
    assert(!store.Resolve(reused, resolved));

    const micropixel_texture_handle_t surface = store.CreateOffscreenSurface(2U, 2U, MICROPIXEL_PIXEL_FORMAT_RGB565);
    assert(surface != 0U);
    assert(store.ResolveMutable(surface, resolved));
    assert(resolved.size == 8U);
    assert(resolved.pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB565);
    assert(resolved.flags == MICROPIXEL_TEXTURE_FLAG_STREAMING);
    store.Release(surface);

    const micropixel_texture_handle_t large_surface =
        store.CreateOffscreenSurface(8192U, 1U, MICROPIXEL_PIXEL_FORMAT_BGRA8888);
    assert(large_surface != 0U);
    assert(store.ResolveMutable(large_surface, resolved));
    assert(resolved.width == 8192U);
    assert(resolved.height == 1U);
    assert(resolved.stride == 32768U);
    store.Release(large_surface);

    std::array<micropixel_texture_handle_t, kMaxBitmaps> handles{};
    for (auto& handle : handles) {
        handle = store.Add(view, false);
        assert(handle != 0U);
    }
    assert(store.Add(view, false) == 0U);
    assert(store.HighWaterMark() == kMaxBitmaps);

    const micropixel::device::BitmapView truncated{
        pixels.data(), 11U, 2U, 2U, 6U, MICROPIXEL_PIXEL_FORMAT_BGR888, 0U,
    };
    assert(store.Add(truncated, false) == 0U);

    store.ReleaseAll();
    for (const auto handle : handles) {
        assert(!store.Resolve(handle, resolved));
    }
    return 0;
}
