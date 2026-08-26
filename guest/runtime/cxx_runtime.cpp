#include <stddef.h>
#include <stdint.h>

#include <__config>
#include <new>

#include "abi/micropixel_abi.h"
#include "runtime/panic.hpp"

namespace {

constexpr uint32_t kWasmPageBytes = 64U * 1024U;
constexpr uint32_t kBlockAlignment = 16U;
constexpr uint32_t kFreeMagic = 0x46524545U;
constexpr uint32_t kAllocatedMagic = 0x414C4C4FU;

struct alignas(kBlockAlignment) Block final {
    uint32_t size;
    uint32_t previous_size;
    uint32_t payload_offset;
    uint32_t magic;
};

static_assert(sizeof(Block) == kBlockAlignment);

extern "C" uint8_t __heap_base;

uint8_t* heap_begin{};
uint8_t* heap_end{};
bool heap_initialized{};

[[nodiscard]] constexpr bool IsPowerOfTwo(size_t value) { return value != 0U && (value & (value - 1U)) == 0U; }

[[nodiscard]] constexpr uintptr_t AlignUp(uintptr_t value, size_t alignment) {
    return (value + alignment - 1U) & ~(static_cast<uintptr_t>(alignment) - 1U);
}

[[nodiscard]] uint8_t* CurrentMemoryEnd() {
    const size_t pages = __builtin_wasm_memory_size(0);
    if (pages > UINTPTR_MAX / kWasmPageBytes) {
        micropixel::runtime::Panic("cxx.heap.size", MICROPIXEL_STATUS_INTERNAL);
    }
    return reinterpret_cast<uint8_t*>(pages * kWasmPageBytes);
}

[[nodiscard]] bool GrowLinearMemory(size_t minimum_extra_bytes) {
    if (minimum_extra_bytes > UINTPTR_MAX - (kWasmPageBytes - 1U)) {
        return false;
    }
    size_t pages = (minimum_extra_bytes + kWasmPageBytes - 1U) / kWasmPageBytes;
    if (pages == 0U) {
        pages = 1U;
    }
    return __builtin_wasm_memory_grow(0, pages) != static_cast<size_t>(-1);
}

[[nodiscard]] Block* FirstBlock() { return reinterpret_cast<Block*>(heap_begin); }

[[nodiscard]] Block* NextBlock(Block* block) {
    auto* next = reinterpret_cast<uint8_t*>(block) + block->size;
    return next < heap_end ? reinterpret_cast<Block*>(next) : nullptr;
}

[[nodiscard]] bool IsValidBlock(const Block* block) {
    const auto* address = reinterpret_cast<const uint8_t*>(block);
    return address >= heap_begin && address + sizeof(Block) <= heap_end && block->size >= sizeof(Block) &&
           block->size % kBlockAlignment == 0U && address + block->size <= heap_end &&
           (block->magic == kFreeMagic || block->magic == kAllocatedMagic);
}

void InitializeHeap() {
    if (heap_initialized) {
        return;
    }
    heap_begin = reinterpret_cast<uint8_t*>(AlignUp(reinterpret_cast<uintptr_t>(&__heap_base), kBlockAlignment));
    heap_end = CurrentMemoryEnd();
    constexpr size_t kMinimumInitialBytes = sizeof(Block) + kBlockAlignment;
    if (heap_end < heap_begin || static_cast<size_t>(heap_end - heap_begin) < kMinimumInitialBytes) {
        const size_t missing = heap_end < heap_begin
                                   ? static_cast<size_t>(heap_begin - heap_end) + kMinimumInitialBytes
                                   : kMinimumInitialBytes - static_cast<size_t>(heap_end - heap_begin);
        if (!GrowLinearMemory(missing)) {
            micropixel::runtime::Panic("cxx.heap.init", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        }
        heap_end = CurrentMemoryEnd();
    }
    const size_t initial_size = static_cast<size_t>(heap_end - heap_begin);
    if (initial_size > UINT32_MAX || initial_size % kBlockAlignment != 0U) {
        micropixel::runtime::Panic("cxx.heap.size", MICROPIXEL_STATUS_INTERNAL);
    }
    *FirstBlock() = Block{
        .size = static_cast<uint32_t>(initial_size), .previous_size = 0U, .payload_offset = 0U, .magic = kFreeMagic};
    heap_initialized = true;
}

void UpdateFollowingPreviousSize(Block* block) {
    if (Block* following = NextBlock(block); following != nullptr) {
        following->previous_size = block->size;
    }
}

[[nodiscard]] bool GrowHeap(size_t minimum_extra_bytes) {
    Block* last = FirstBlock();
    if (!IsValidBlock(last)) {
        micropixel::runtime::Panic("cxx.heap.corrupt", MICROPIXEL_STATUS_INTERNAL);
    }
    for (Block* next = NextBlock(last); next != nullptr; next = NextBlock(last)) {
        if (!IsValidBlock(next)) {
            micropixel::runtime::Panic("cxx.heap.corrupt", MICROPIXEL_STATUS_INTERNAL);
        }
        last = next;
    }
    uint8_t* previous_end = heap_end;
    if (reinterpret_cast<uint8_t*>(last) + last->size != previous_end) {
        micropixel::runtime::Panic("cxx.heap.corrupt", MICROPIXEL_STATUS_INTERNAL);
    }
    if (!GrowLinearMemory(minimum_extra_bytes)) {
        return false;
    }
    heap_end = CurrentMemoryEnd();
    if (heap_end <= previous_end || static_cast<size_t>(heap_end - previous_end) > UINT32_MAX) {
        micropixel::runtime::Panic("cxx.heap.grow", MICROPIXEL_STATUS_INTERNAL);
    }
    const uint32_t extension = static_cast<uint32_t>(heap_end - previous_end);
    if (last->magic == kFreeMagic) {
        if (last->size > UINT32_MAX - extension) {
            micropixel::runtime::Panic("cxx.heap.size", MICROPIXEL_STATUS_INTERNAL);
        }
        last->size += extension;
        return true;
    }
    *reinterpret_cast<Block*>(previous_end) =
        Block{.size = extension, .previous_size = last->size, .payload_offset = 0U, .magic = kFreeMagic};
    return true;
}

[[nodiscard]] bool AllocationFootprint(size_t requested_size, size_t alignment, size_t& footprint_out) {
    constexpr size_t kFixedOverhead = sizeof(Block) + sizeof(uintptr_t) + kBlockAlignment - 1U;
    if (alignment > UINT32_MAX || requested_size > UINT32_MAX - kFixedOverhead ||
        requested_size + kFixedOverhead > UINT32_MAX - (alignment - 1U)) {
        return false;
    }
    footprint_out = requested_size + kFixedOverhead + alignment - 1U;
    return true;
}

[[nodiscard]] void* Allocate(size_t requested_size, size_t alignment) {
    if (requested_size == 0U) {
        requested_size = 1U;
    }
    if (alignment < alignof(uintptr_t)) {
        alignment = alignof(uintptr_t);
    }
    size_t footprint = 0U;
    if (!IsPowerOfTwo(alignment) || !AllocationFootprint(requested_size, alignment, footprint)) {
        return nullptr;
    }

    InitializeHeap();
    for (;;) {
        for (Block* block = FirstBlock(); block != nullptr; block = NextBlock(block)) {
            if (!IsValidBlock(block)) {
                micropixel::runtime::Panic("cxx.heap.corrupt", MICROPIXEL_STATUS_INTERNAL);
            }
            if (block->magic != kFreeMagic) {
                continue;
            }
            const uintptr_t block_address = reinterpret_cast<uintptr_t>(block);
            const uintptr_t payload = AlignUp(block_address + sizeof(Block) + sizeof(uintptr_t), alignment);
            const uintptr_t allocation_end = AlignUp(payload + requested_size, kBlockAlignment);
            const size_t required = allocation_end - block_address;
            if (required > block->size) {
                continue;
            }

            const uint32_t original_size = block->size;
            const uint32_t remaining = original_size - static_cast<uint32_t>(required);
            if (remaining >= sizeof(Block) + kBlockAlignment) {
                block->size = static_cast<uint32_t>(required);
                auto* split = reinterpret_cast<Block*>(reinterpret_cast<uint8_t*>(block) + block->size);
                *split =
                    Block{.size = remaining, .previous_size = block->size, .payload_offset = 0U, .magic = kFreeMagic};
                UpdateFollowingPreviousSize(split);
            }
            block->payload_offset = static_cast<uint32_t>(payload - block_address);
            block->magic = kAllocatedMagic;
            *reinterpret_cast<Block**>(payload - sizeof(Block*)) = block;
            return reinterpret_cast<void*>(payload);
        }
        if (!GrowHeap(footprint)) {
            return nullptr;
        }
    }
}

void Release(void* memory) {
    if (memory == nullptr) {
        return;
    }
    auto* address = static_cast<uint8_t*>(memory);
    if (address < heap_begin + sizeof(Block) + sizeof(Block*) || address >= heap_end) {
        micropixel::runtime::Panic("cxx.delete.pointer", MICROPIXEL_STATUS_INVALID_MEMORY);
    }
    Block* block = *(reinterpret_cast<Block**>(address) - 1U);
    if (!IsValidBlock(block) || block->magic != kAllocatedMagic ||
        reinterpret_cast<uint8_t*>(block) + block->payload_offset != address) {
        micropixel::runtime::Panic("cxx.delete.pointer", MICROPIXEL_STATUS_INVALID_MEMORY);
    }

    block->payload_offset = 0U;
    block->magic = kFreeMagic;
    if (Block* next = NextBlock(block); next != nullptr && IsValidBlock(next) && next->magic == kFreeMagic) {
        block->size += next->size;
        UpdateFollowingPreviousSize(block);
    }
    if (block->previous_size != 0U) {
        auto* previous = reinterpret_cast<Block*>(reinterpret_cast<uint8_t*>(block) - block->previous_size);
        if (IsValidBlock(previous) && previous->magic == kFreeMagic && NextBlock(previous) == block) {
            previous->size += block->size;
            UpdateFollowingPreviousSize(previous);
        }
    }
}

[[nodiscard]] void* AllocateOrPanic(size_t size, size_t alignment) {
    if (void* memory = Allocate(size, alignment); memory != nullptr) {
        return memory;
    }
    micropixel::runtime::Panic("cxx.new.oom", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
}

}  // namespace

void* operator new(size_t size) { return AllocateOrPanic(size, alignof(max_align_t)); }

void* operator new[](size_t size) { return AllocateOrPanic(size, alignof(max_align_t)); }

void* operator new(size_t size, const std::nothrow_t&) noexcept { return Allocate(size, alignof(max_align_t)); }

void* operator new[](size_t size, const std::nothrow_t&) noexcept { return Allocate(size, alignof(max_align_t)); }

void* operator new(size_t size, std::align_val_t alignment) {
    return AllocateOrPanic(size, static_cast<size_t>(alignment));
}

void* operator new[](size_t size, std::align_val_t alignment) {
    return AllocateOrPanic(size, static_cast<size_t>(alignment));
}

void* operator new(size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return Allocate(size, static_cast<size_t>(alignment));
}

void* operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return Allocate(size, static_cast<size_t>(alignment));
}

void operator delete(void* memory) noexcept { Release(memory); }

void operator delete[](void* memory) noexcept { Release(memory); }

void operator delete(void* memory, size_t) noexcept { Release(memory); }

void operator delete[](void* memory, size_t) noexcept { Release(memory); }

void operator delete(void* memory, const std::nothrow_t&) noexcept { Release(memory); }

void operator delete[](void* memory, const std::nothrow_t&) noexcept { Release(memory); }

void operator delete(void* memory, std::align_val_t) noexcept { Release(memory); }

void operator delete[](void* memory, std::align_val_t) noexcept { Release(memory); }

void operator delete(void* memory, size_t, std::align_val_t) noexcept { Release(memory); }

void operator delete[](void* memory, size_t, std::align_val_t) noexcept { Release(memory); }

void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept { Release(memory); }

void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept { Release(memory); }

extern "C" [[noreturn]] void __cxa_pure_virtual() {
    micropixel::runtime::Panic("cxx.pure_virtual", MICROPIXEL_STATUS_INTERNAL);
}

extern "C" [[noreturn]] void __cxa_deleted_virtual() {
    micropixel::runtime::Panic("cxx.deleted_virtual", MICROPIXEL_STATUS_INTERNAL);
}

_LIBCPP_BEGIN_NAMESPACE_STD

[[noreturn]] void __libcpp_verbose_abort(const char*, ...) noexcept {
    micropixel::runtime::Panic("cxx.libcpp.abort", MICROPIXEL_STATUS_INVALID_ARGUMENT);
}

_LIBCPP_END_NAMESPACE_STD
