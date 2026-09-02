#include "runtime/resources/bitmap_store.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

namespace micropixel::runtime {

BitmapStore::BitmapStore()
    : slots_(static_cast<Slot*>(
          heap_caps_malloc(limits::kMaxBitmaps * sizeof(Slot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {
    if (slots_ != nullptr) {
        std::uninitialized_value_construct_n(slots_, limits::kMaxBitmaps);
    }
}

BitmapStore::~BitmapStore() {
    ReleaseAll();
    std::destroy_n(slots_, slots_ == nullptr ? 0U : limits::kMaxBitmaps);
    heap_caps_free(slots_);
}

micropixel_texture_handle_t BitmapStore::MakeHandle(uint32_t index, uint32_t generation) {
    return (generation << kHandleIndexBits) | (index + 1U);
}

BitmapStore::Slot* BitmapStore::ResolveSlotLocked(micropixel_texture_handle_t bitmap) {
    return const_cast<Slot*>(std::as_const(*this).ResolveSlotLocked(bitmap));
}

const BitmapStore::Slot* BitmapStore::ResolveSlotLocked(micropixel_texture_handle_t bitmap) const {
    const uint32_t encoded_index = bitmap & kHandleIndexMask;
    const uint32_t generation = bitmap >> kHandleIndexBits;
    if (slots_ == nullptr || encoded_index == 0U || encoded_index > limits::kMaxBitmaps || generation == 0U) {
        return nullptr;
    }
    const Slot& slot = slots_[encoded_index - 1U];
    return slot.data != nullptr && slot.generation == generation ? &slot : nullptr;
}

device::BitmapView BitmapStore::View(const Slot& slot) {
    return device::BitmapView{
        slot.data,         static_cast<uint32_t>(slot.stride) * slot.height,   slot.width, slot.height, slot.stride,
        slot.pixel_format, static_cast<uint32_t>(slot.flags & kPublicFlagMask)};
}

void BitmapStore::ClearSlot(Slot& slot) {
    const uint32_t generation = slot.generation;
    slot = {};
    slot.generation = generation;
}

micropixel_texture_handle_t BitmapStore::Add(const device::BitmapView& view, bool owned, bool mutable_pixels) {
    const uint64_t required_size = static_cast<uint64_t>(view.stride) * view.height;
    if (slots_ == nullptr || view.data == nullptr || view.width == 0U || view.height == 0U || view.stride == 0U ||
        view.width > UINT16_MAX || view.height > UINT16_MAX || view.stride > UINT16_MAX || required_size == 0U ||
        required_size > view.size || view.pixel_format > UINT8_MAX || (view.flags & ~kPublicFlagMask) != 0U) {
        return 0U;
    }
    portENTER_CRITICAL(&lock_);
    for (uint32_t index = 0U; index < limits::kMaxBitmaps; ++index) {
        Slot& slot = slots_[index];
        if (slot.data != nullptr) {
            continue;
        }
        uint32_t generation = (slot.generation + 1U) & kHandleGenerationMask;
        if (generation == 0U) {
            generation = 1U;
        }
        slot = {
            .data = view.data,
            .generation = generation,
            .width = static_cast<uint16_t>(view.width),
            .height = static_cast<uint16_t>(view.height),
            .stride = static_cast<uint16_t>(view.stride),
            .pixel_format = static_cast<uint8_t>(view.pixel_format),
            .flags = static_cast<uint8_t>(view.flags | kGuestReference |
                                          (owned ? static_cast<uint32_t>(kOwned) : 0U) |
                                          (mutable_pixels ? static_cast<uint32_t>(kMutablePixels) : 0U)),
        };
        ++live_count_;
        high_water_mark_ = std::max(high_water_mark_, live_count_);
        const micropixel_texture_handle_t handle = MakeHandle(index, generation);
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
                                         : (pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888
                                                ? 4U
                                                : (pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB565 ? 2U : 0U));
    const uint64_t stride = static_cast<uint64_t>(width) * bytes_per_pixel;
    const uint64_t size = stride * height;
    if (width == 0U || height == 0U || width > UINT16_MAX || height > UINT16_MAX || bytes_per_pixel == 0U ||
        stride > UINT16_MAX || size > UINT32_MAX) {
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
        heap_caps_free(pixels);
    }
    return handle;
}

bool BitmapStore::Resolve(micropixel_texture_handle_t bitmap, device::BitmapView& view_out) const {
    bool found = false;
    portENTER_CRITICAL(&lock_);
    const Slot* slot = ResolveSlotLocked(bitmap);
    if (slot != nullptr && (slot->flags & kGuestReference) != 0U) {
        view_out = View(*slot);
        found = true;
    }
    portEXIT_CRITICAL(&lock_);
    return found;
}

bool BitmapStore::ResolveMutable(micropixel_texture_handle_t bitmap, device::BitmapView& view_out) const {
    bool found = false;
    portENTER_CRITICAL(&lock_);
    const Slot* slot = ResolveSlotLocked(bitmap);
    if (slot != nullptr && (slot->flags & (kGuestReference | kMutablePixels)) == (kGuestReference | kMutablePixels)) {
        view_out = View(*slot);
        found = true;
    }
    portEXIT_CRITICAL(&lock_);
    return found;
}

bool BitmapStore::RetainSceneReference(micropixel_texture_handle_t bitmap) {
    bool retained = false;
    portENTER_CRITICAL(&lock_);
    Slot* slot = ResolveSlotLocked(bitmap);
    if (slot != nullptr && (slot->flags & kGuestReference) != 0U && slot->scene_references != UINT32_MAX) {
        ++slot->scene_references;
        retained = true;
    }
    portEXIT_CRITICAL(&lock_);
    return retained;
}

void BitmapStore::ReleaseSceneReference(micropixel_texture_handle_t bitmap) {
    const uint8_t* owned_data = nullptr;
    portENTER_CRITICAL(&lock_);
    Slot* slot = ResolveSlotLocked(bitmap);
    if (slot != nullptr && slot->scene_references != 0U) {
        --slot->scene_references;
        if (slot->scene_references == 0U && (slot->flags & kGuestReference) == 0U) {
            if ((slot->flags & kOwned) != 0U) {
                owned_data = slot->data;
            }
            ClearSlot(*slot);
            --live_count_;
        }
    }
    portEXIT_CRITICAL(&lock_);
    heap_caps_free(const_cast<uint8_t*>(owned_data));
}

void BitmapStore::Release(micropixel_texture_handle_t bitmap) {
    const uint8_t* owned_data = nullptr;
    portENTER_CRITICAL(&lock_);
    Slot* slot = ResolveSlotLocked(bitmap);
    if (slot != nullptr) {
        slot->flags &= static_cast<uint8_t>(~kGuestReference);
        if (slot->scene_references == 0U) {
            if ((slot->flags & kOwned) != 0U) {
                owned_data = slot->data;
            }
            ClearSlot(*slot);
            --live_count_;
        }
    }
    portEXIT_CRITICAL(&lock_);
    heap_caps_free(const_cast<uint8_t*>(owned_data));
}

void BitmapStore::ReleaseAll() {
    const uint8_t* owned_data[limits::kMaxBitmaps]{};
    uint32_t owned_count = 0U;
    portENTER_CRITICAL(&lock_);
    for (uint32_t index = 0U; slots_ != nullptr && index < limits::kMaxBitmaps; ++index) {
        Slot& slot = slots_[index];
        if (slot.data != nullptr && (slot.flags & kOwned) != 0U) {
            owned_data[owned_count++] = slot.data;
        }
        slot = {};
    }
    live_count_ = 0U;
    portEXIT_CRITICAL(&lock_);
    for (uint32_t index = 0U; index < owned_count; ++index) {
        heap_caps_free(const_cast<uint8_t*>(owned_data[index]));
    }
}

uint32_t BitmapStore::HighWaterMark() const {
    portENTER_CRITICAL(&lock_);
    const uint32_t high_water_mark = high_water_mark_;
    portEXIT_CRITICAL(&lock_);
    return high_water_mark;
}

}  // namespace micropixel::runtime
