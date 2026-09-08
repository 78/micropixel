#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "abi/micropixel_abi.h"
#include "device/contracts/graphics.hpp"
#include "runtime/resources/bitmap_store.hpp"
#include "runtime/runtime_limits.hpp"

namespace {
bool fail_allocation{};
std::unordered_set<void*> allocations;
}  // namespace
void* micropixel_test_psram_allocate(size_t size) {
    if (fail_allocation) return nullptr;
    void* memory = std::malloc(size);
    if (memory) allocations.insert(memory);
    return memory;
}
void micropixel_test_psram_free(void* memory) {
    if (!memory) return;
    assert(allocations.erase(memory) == 1);
    std::free(memory);
}

namespace {

micropixel::device::BitmapView MakeView(const uint8_t* pixels) {
    return micropixel::device::BitmapView{
        pixels, 12U, 2U, 2U, 6U, MICROPIXEL_PIXEL_FORMAT_BGR888, 0U,
    };
}

}  // namespace

void DynamicSnapshotsRetainOldPixelsAndRejectInvalidUpdates() {
    using namespace micropixel::runtime;
    using micropixel::device::BitmapView;
    {
        BitmapStore store;
        const uint8_t pixels[]{1, 2, 3, 4, 90, 90, 5, 6, 7, 8};
        const auto initial = store.CreateDynamic(2, 2, MICROPIXEL_PIXEL_FORMAT_RGB565, pixels, sizeof(pixels), 6);
        assert(initial);
        BitmapView before{};
        assert(store.Resolve(*initial, before) && before.size == 8);
        const uint8_t expected[]{1, 2, 3, 4, 5, 6, 7, 8};
        assert(std::memcmp(before.data, expected, 8) == 0);
        assert(store.RetainSceneReference(*initial));
        const uint8_t patch[]{11, 12};
        assert(!store.UpdateDynamic(*initial, UINT32_MAX, 0, 1, 1, patch, 2, 2));
        assert(!store.UpdateDynamic(*initial, 0, 0, 1, 1, patch, 1, 2));
        assert(!store.UpdateDynamic(*initial, 0, 0, 1, 1, patch, 2, 1));
        assert(!store.UpdateDynamic(*initial, 0, 0, 1, 1, nullptr, 2, 2));
        const auto allocated = allocations.size();
        fail_allocation = true;
        auto failed = store.UpdateDynamic(*initial, 1, 0, 1, 1, patch, 2, 2);
        assert(!failed && failed.error().status == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        fail_allocation = false;
        assert(allocations.size() == allocated && std::memcmp(before.data, expected, 8) == 0);
        auto updated = store.UpdateDynamic(*initial, 1, 0, 1, 1, patch, 2, 2);
        assert(updated && *updated != *initial);
        BitmapView after{};
        assert(store.Resolve(*updated, after));
        assert(after.data[2] == 11 && after.data[3] == 12 && after.data[6] == 7);
        assert(std::memcmp(before.data, expected, 8) == 0);
        store.Release(*initial);
        assert(!store.Resolve(*initial, after));
        // A previously accepted frame has an independent pixel reference.
        assert(std::memcmp(before.data, expected, 8) == 0);
        assert(!store.UpdateDynamic(*initial, 0, 0, 1, 1, patch, 2, 2));
        store.ReleaseSceneReference(*initial);
        const auto retained_allocations = allocations.size();
        // Exhaust handles after retaining an updateable source. Failed Add must
        // free its candidate allocation and keep the old snapshot unchanged.
        uint8_t immutable_pixels[12]{};
        while (store.Add(MakeView(immutable_pixels), false)) {
        }
        auto full = store.UpdateDynamic(*updated, 0, 0, 1, 1, patch, 2, 2);
        assert(!full && full.error().status == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        assert(allocations.size() == retained_allocations);
        assert(store.Resolve(*updated, after) && after.data[0] == 1);
    }
    assert(allocations.empty());
}

int main() {
    DynamicSnapshotsRetainOldPixelsAndRejectInvalidUpdates();
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

    const micropixel_texture_handle_t reused = store.Add(view, false);
    assert(reused != 0U);
    assert(reused != first);
    assert(!store.Resolve(first, resolved));
    assert(store.Resolve(reused, resolved));

    assert(store.RetainSceneReference(reused));
    store.Release(reused);
    assert(!store.Resolve(reused, resolved));
    store.ReleaseSceneReference(reused);
    assert(!store.Resolve(reused, resolved));

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
