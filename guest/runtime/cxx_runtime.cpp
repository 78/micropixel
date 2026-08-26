#include <stddef.h>
#include <stdint.h>

#include <__config>
#include <new>

#include "abi/micropixel_abi.h"
#include "runtime/panic.hpp"

namespace {

constexpr uint32_t kHeapBytes = 32U * 1024U;
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

alignas(kBlockAlignment) uint8_t heap[kHeapBytes]{};
bool heap_initialized{};

[[nodiscard]] constexpr bool IsPowerOfTwo(size_t value) { return value != 0U && (value & (value - 1U)) == 0U; }

[[nodiscard]] constexpr uintptr_t AlignUp(uintptr_t value, size_t alignment) {
    return (value + alignment - 1U) & ~(static_cast<uintptr_t>(alignment) - 1U);
}

[[nodiscard]] Block* FirstBlock() { return reinterpret_cast<Block*>(heap); }

[[nodiscard]] uint8_t* HeapEnd() { return heap + sizeof(heap); }

void InitializeHeap() {
    if (heap_initialized) {
        return;
    }
    *FirstBlock() = Block{.size = sizeof(heap), .previous_size = 0U, .payload_offset = 0U, .magic = kFreeMagic};
    heap_initialized = true;
}

[[nodiscard]] Block* NextBlock(Block* block) {
    auto* next = reinterpret_cast<uint8_t*>(block) + block->size;
    return next < HeapEnd() ? reinterpret_cast<Block*>(next) : nullptr;
}

[[nodiscard]] bool IsValidBlock(const Block* block) {
    const auto* address = reinterpret_cast<const uint8_t*>(block);
    return address >= heap && address + sizeof(Block) <= HeapEnd() && block->size >= sizeof(Block) &&
           block->size % kBlockAlignment == 0U && address + block->size <= HeapEnd() &&
           (block->magic == kFreeMagic || block->magic == kAllocatedMagic);
}

void UpdateFollowingPreviousSize(Block* block) {
    if (Block* following = NextBlock(block); following != nullptr) {
        following->previous_size = block->size;
    }
}

[[nodiscard]] void* Allocate(size_t requested_size, size_t alignment) {
    if (requested_size == 0U) {
        requested_size = 1U;
    }
    if (alignment < alignof(uintptr_t)) {
        alignment = alignof(uintptr_t);
    }
    if (!IsPowerOfTwo(alignment) || requested_size > kHeapBytes || alignment > kHeapBytes) {
        return nullptr;
    }

    InitializeHeap();
    for (Block* block = FirstBlock(); block != nullptr; block = NextBlock(block)) {
        if (!IsValidBlock(block)) {
            micropixel::runtime::Panic("cxx.heap.corrupt", MICROPIXEL_STATUS_INTERNAL);
        }
        if (block->magic != kFreeMagic) {
            continue;
        }
        const uintptr_t block_address = reinterpret_cast<uintptr_t>(block);
        const uintptr_t payload = AlignUp(block_address + sizeof(Block) + sizeof(uintptr_t), alignment);
        if (requested_size > kHeapBytes - (payload - block_address)) {
            continue;
        }
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
            *split = Block{.size = remaining, .previous_size = block->size, .payload_offset = 0U, .magic = kFreeMagic};
            UpdateFollowingPreviousSize(split);
        }
        block->payload_offset = static_cast<uint32_t>(payload - block_address);
        block->magic = kAllocatedMagic;
        *reinterpret_cast<Block**>(payload - sizeof(Block*)) = block;
        return reinterpret_cast<void*>(payload);
    }
    return nullptr;
}

void Release(void* memory) {
    if (memory == nullptr) {
        return;
    }
    auto* address = static_cast<uint8_t*>(memory);
    if (address < heap + sizeof(Block) + sizeof(Block*) || address >= HeapEnd()) {
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
