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
        slot.data,         static_cast<uint32_t>(slot.stride) * slot.height, slot.width, slot.height, slot.stride,
        slot.pixel_format, static_cast<uint32_t>(slot.flags & kViewFlagMask)};
}

void BitmapStore::ClearSlot(Slot& slot) {
    const uint32_t generation = slot.generation;
    slot = {};
    slot.generation = generation;
}

micropixel_texture_handle_t BitmapStore::Add(const device::BitmapView& view, bool owned) {
    const uint64_t required_size = static_cast<uint64_t>(view.stride) * view.height;
    if (slots_ == nullptr || view.data == nullptr || view.width == 0U || view.height == 0U || view.stride == 0U ||
        view.width > UINT16_MAX || view.height > UINT16_MAX || view.stride > UINT16_MAX || required_size == 0U ||
        required_size > view.size || view.pixel_format > UINT8_MAX || (view.flags & ~kViewFlagMask) != 0U) {
        return 0U;
    }
    portENTER_CRITICAL(&lock_);
    for (uint32_t index = 0U; index < limits::kMaxBitmaps; ++index) {
        Slot& slot = slots_[index];
        if (slot.data != nullptr || slot.generation == kHandleGenerationMask) {
            continue;
        }
        const uint32_t generation = slot.generation + 1U;
        slot = {
            .data = view.data,
            .generation = generation,
            .width = static_cast<uint16_t>(view.width),
            .height = static_cast<uint16_t>(view.height),
            .stride = static_cast<uint16_t>(view.stride),
            .pixel_format = static_cast<uint8_t>(view.pixel_format),
            .flags = static_cast<uint8_t>(view.flags | kGuestReference | (owned ? static_cast<uint32_t>(kOwned) : 0U)),
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

namespace {
uint32_t PixelBytes(uint32_t format) {
    switch (format) {
        case MICROPIXEL_PIXEL_FORMAT_RGB565:
            return 2;
        case MICROPIXEL_PIXEL_FORMAT_BGR888:
            return 3;
        case MICROPIXEL_PIXEL_FORMAT_BGRA8888:
            return 4;
        default:
            return 0;
    }
}
bool ValidPixels(uint32_t width, uint32_t height, uint32_t bytes, const uint8_t* pixels, uint32_t length,
                 uint32_t pitch) {
    const uint64_t row = static_cast<uint64_t>(width) * bytes;
    return width && height && bytes && pixels && pitch >= row &&
           static_cast<uint64_t>(height - 1) * pitch + row <= length;
}
}  // namespace

ServiceResult<micropixel_texture_handle_t> BitmapStore::CreateDynamic(uint32_t width, uint32_t height, uint32_t format,
                                                                      const uint8_t* pixels, uint32_t length,
                                                                      uint32_t pitch) {
    const uint32_t bytes = PixelBytes(format);
    const uint64_t stride = static_cast<uint64_t>(width) * bytes;
    const uint64_t size = stride * height;
    const bool empty = pixels == nullptr && length == 0 && pitch == 0;
    if (!width || !height || width > UINT16_MAX || height > UINT16_MAX || !bytes || stride > UINT16_MAX ||
        size > UINT32_MAX || (!empty && !ValidPixels(width, height, bytes, pixels, length, pitch)))
        return FailService<micropixel_texture_handle_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    auto* data = static_cast<uint8_t*>(heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data) return FailService<micropixel_texture_handle_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    if (empty)
        std::memset(data, 0, size);
    else
        for (uint32_t row = 0; row < height; ++row)
            std::memcpy(data + row * stride, pixels + static_cast<size_t>(row) * pitch, stride);
    const device::BitmapView view{data,   static_cast<uint32_t>(size),    width, height, static_cast<uint32_t>(stride),
                                  format, MICROPIXEL_TEXTURE_FLAG_DYNAMIC};
    const auto handle = Add(view, true);
    if (!handle) {
        heap_caps_free(data);
        return FailService<micropixel_texture_handle_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    return handle;
}

ServiceResult<micropixel_texture_handle_t> BitmapStore::UpdateDynamic(micropixel_texture_handle_t source, uint32_t x,
                                                                      uint32_t y, uint32_t width, uint32_t height,
                                                                      const uint8_t* pixels, uint32_t length,
                                                                      uint32_t pitch) {
    device::BitmapView old{};
    if (!Resolve(source, old) || (old.flags & MICROPIXEL_TEXTURE_FLAG_DYNAMIC) == 0 ||
        static_cast<uint64_t>(x) + width > old.width || static_cast<uint64_t>(y) + height > old.height ||
        !ValidPixels(width, height, PixelBytes(old.pixel_format), pixels, length, pitch))
        return FailService<micropixel_texture_handle_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    auto* data = static_cast<uint8_t*>(heap_caps_aligned_alloc(64, old.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data) return FailService<micropixel_texture_handle_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    std::memcpy(data, old.data, old.size);
    const uint32_t bytes = PixelBytes(old.pixel_format);
    for (uint32_t row = 0; row < height; ++row)
        std::memcpy(data + static_cast<size_t>(y + row) * old.stride + x * bytes,
                    pixels + static_cast<size_t>(row) * pitch, width * bytes);
    old.data = data;
    const auto handle = Add(old, true);
    if (!handle) {
        heap_caps_free(data);
        return FailService<micropixel_texture_handle_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
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

bool BitmapStore::SetRgb565ByteOrder(micropixel_texture_handle_t bitmap, bool swapped) {
    constexpr uint8_t kSwappedFlag = device::bitmap_flags::kRgb565ByteSwapped;
    uint8_t* data = nullptr;
    uint32_t width = 0U;
    uint32_t height = 0U;
    uint32_t stride = 0U;
    bool convertible = false;
    portENTER_CRITICAL(&lock_);
    const Slot* slot = ResolveSlotLocked(bitmap);
    if (slot != nullptr && slot->pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB565) {
        if (((slot->flags & kSwappedFlag) != 0U) == swapped) {
            portEXIT_CRITICAL(&lock_);
            return true;
        }
        // Flash-mapped assets cannot be rewritten; dynamic bitmaps receive
        // canonical rows from the Guest and stay canonical.
        convertible = (slot->flags & kOwned) != 0U && (slot->flags & MICROPIXEL_TEXTURE_FLAG_DYNAMIC) == 0U;
        data = const_cast<uint8_t*>(slot->data);
        width = slot->width;
        height = slot->height;
        stride = slot->stride;
    }
    portEXIT_CRITICAL(&lock_);
    if (!convertible) {
        return false;
    }
    for (uint32_t row = 0U; row < height; ++row) {
        auto* pixels = reinterpret_cast<uint16_t*>(data + static_cast<size_t>(row) * stride);
        for (uint32_t x = 0U; x < width; ++x) {
            pixels[x] = static_cast<uint16_t>((pixels[x] << 8U) | (pixels[x] >> 8U));
        }
    }
    bool recorded = false;
    portENTER_CRITICAL(&lock_);
    Slot* mutable_slot = ResolveSlotLocked(bitmap);
    if (mutable_slot != nullptr && mutable_slot->data == data) {
        mutable_slot->flags = static_cast<uint8_t>(swapped ? (mutable_slot->flags | kSwappedFlag)
                                                           : (mutable_slot->flags & ~kSwappedFlag));
        recorded = true;
    }
    portEXIT_CRITICAL(&lock_);
    return recorded;
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
        ClearSlot(slot);
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
