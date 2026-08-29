#include "runtime/memory/guest_psram.hpp"

#include "esp_heap_caps.h"
#include "sdkconfig.h"

namespace micropixel::runtime {
namespace {

constexpr uint32_t kPsramCapabilities = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

template <typename Allocate>
void* AllocateWhilePreservingReserve(size_t bytes, Allocate allocate) {
    if (!IsGuestPsramAllocationAdmissible(CurrentGuestPsramState(), bytes, GuestPsramReserveBytes())) {
        return nullptr;
    }
    void* memory = allocate();
    if (memory != nullptr && heap_caps_get_free_size(kPsramCapabilities) < GuestPsramReserveBytes()) {
        heap_caps_free(memory);
        return nullptr;
    }
    return memory;
}

}  // namespace

size_t GuestPsramReserveBytes() { return CONFIG_MICROPIXEL_GUEST_PSRAM_RESERVE_BYTES; }

GuestPsramState CurrentGuestPsramState() {
    return GuestPsramState{.free_bytes = heap_caps_get_free_size(kPsramCapabilities),
                           .largest_free_block = heap_caps_get_largest_free_block(kPsramCapabilities)};
}

bool CanGrowGuestPsram(size_t additional_bytes) {
    return IsGuestPsramGrowthAdmissible(heap_caps_get_free_size(kPsramCapabilities), additional_bytes,
                                        GuestPsramReserveBytes());
}

void* AllocateGuestPsram(size_t bytes) {
    return AllocateWhilePreservingReserve(bytes, [bytes]() { return heap_caps_malloc(bytes, kPsramCapabilities); });
}

void* AllocateAlignedGuestPsram(size_t alignment, size_t bytes) {
    return AllocateWhilePreservingReserve(
        bytes, [alignment, bytes]() { return heap_caps_aligned_alloc(alignment, bytes, kPsramCapabilities); });
}

}  // namespace micropixel::runtime
