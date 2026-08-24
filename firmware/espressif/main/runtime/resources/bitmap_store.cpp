#include "runtime/resources/bitmap_store.hpp"

#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

namespace micropixel::runtime {

BitmapStore::~BitmapStore() { ReleaseAll(); }

micropixel_texture_handle_t BitmapStore::Add(const device::BitmapView& view, bool owned, bool mutable_pixels) {
    if (view.data == nullptr || view.size == 0U) {
        return 0U;
    }
    portENTER_CRITICAL(&lock_);
    if (owned && (view.size > CONFIG_MICROPIXEL_BITMAP_PSRAM_QUOTA_BYTES ||
                  owned_bytes_ > CONFIG_MICROPIXEL_BITMAP_PSRAM_QUOTA_BYTES - view.size)) {
        portEXIT_CRITICAL(&lock_);
        return 0U;
    }
    for (auto& slot : slots_) {
        if (slot.handle != 0U) {
            continue;
        }
        uint32_t handle = next_handle_++;
        if (handle == 0U) {
            handle = next_handle_++;
        }
        slot = {
            .handle = handle,
            .view = view,
            .owned = owned,
            .mutable_pixels = mutable_pixels,
            .guest_reference = true,
        };
        if (owned) {
            owned_bytes_ += view.size;
        }
        portEXIT_CRITICAL(&lock_);
        return handle;
    }
    portEXIT_CRITICAL(&lock_);
    return 0U;
}

micropixel_texture_handle_t BitmapStore::CreateOffscreenSurface(uint32_t width, uint32_t height,
                                                                uint32_t pixel_format) {
    const uint32_t bytes_per_pixel = pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888
                                         ? 3U
                                         : (pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? 4U : 0U);
    const uint64_t stride = static_cast<uint64_t>(width) * bytes_per_pixel;
    const uint64_t size = stride * height;
    if (width == 0U || height == 0U || width > 720U || height > 720U || bytes_per_pixel == 0U || stride > UINT32_MAX ||
        size > UINT32_MAX || size > CONFIG_MICROPIXEL_BITMAP_PSRAM_QUOTA_BYTES) {
        return 0U;
    }
    auto* pixels = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(64U, static_cast<size_t>(size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr) {
        return 0U;
    }
    std::memset(pixels, 0, static_cast<size_t>(size));
    device::BitmapView view{
        pixels,       static_cast<uint32_t>(size),      width, height, static_cast<uint32_t>(stride),
        pixel_format, MICROPIXEL_TEXTURE_FLAG_STREAMING};
    const micropixel_texture_handle_t handle = Add(view, true, true);
    if (handle == 0U) {
        std::free(pixels);
    }
    return handle;
}

bool BitmapStore::Resolve(micropixel_texture_handle_t bitmap, device::BitmapView& view_out) const {
    bool found = false;
    portENTER_CRITICAL(&lock_);
    for (const auto& slot : slots_) {
        if (slot.handle == bitmap && slot.guest_reference) {
            view_out = slot.view;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&lock_);
    return found;
}

bool BitmapStore::ResolveMutable(micropixel_texture_handle_t bitmap, device::BitmapView& view_out) const {
    bool found = false;
    portENTER_CRITICAL(&lock_);
    for (const auto& slot : slots_) {
        if (slot.handle == bitmap && slot.guest_reference && slot.mutable_pixels) {
            view_out = slot.view;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&lock_);
    return found;
}

bool BitmapStore::RetainSceneReference(micropixel_texture_handle_t bitmap) {
    bool retained = false;
    portENTER_CRITICAL(&lock_);
    for (auto& slot : slots_) {
        if (slot.handle == bitmap && slot.guest_reference && slot.scene_references != UINT32_MAX) {
            ++slot.scene_references;
            retained = true;
            break;
        }
    }
    portEXIT_CRITICAL(&lock_);
    return retained;
}

void BitmapStore::ReleaseSceneReference(micropixel_texture_handle_t bitmap) {
    const uint8_t* owned_data = nullptr;
    portENTER_CRITICAL(&lock_);
    for (auto& slot : slots_) {
        if (slot.handle != bitmap || slot.scene_references == 0U) {
            continue;
        }
        --slot.scene_references;
        if (slot.scene_references == 0U && !slot.guest_reference) {
            if (slot.owned) {
                owned_bytes_ -= slot.view.size;
                owned_data = slot.view.data;
            }
            slot = {};
        }
        break;
    }
    portEXIT_CRITICAL(&lock_);
    std::free(const_cast<uint8_t*>(owned_data));
}

void BitmapStore::Release(micropixel_texture_handle_t bitmap) {
    const uint8_t* owned_data = nullptr;
    portENTER_CRITICAL(&lock_);
    for (auto& slot : slots_) {
        if (slot.handle != bitmap) {
            continue;
        }
        slot.guest_reference = false;
        if (slot.scene_references == 0U) {
            if (slot.owned) {
                owned_bytes_ -= slot.view.size;
                owned_data = slot.view.data;
            }
            slot = {};
        }
        break;
    }
    portEXIT_CRITICAL(&lock_);
    std::free(const_cast<uint8_t*>(owned_data));
}

void BitmapStore::ReleaseAll() {
    const uint8_t* owned_data[limits::kMaxBitmaps]{};
    uint32_t owned_count = 0U;
    portENTER_CRITICAL(&lock_);
    for (auto& slot : slots_) {
        if (slot.handle != 0U && slot.owned) {
            owned_data[owned_count++] = slot.view.data;
        }
        slot = {};
    }
    owned_bytes_ = 0U;
    portEXIT_CRITICAL(&lock_);
    for (uint32_t index = 0U; index < owned_count; ++index) {
        std::free(const_cast<uint8_t*>(owned_data[index]));
    }
}

}  // namespace micropixel::runtime
