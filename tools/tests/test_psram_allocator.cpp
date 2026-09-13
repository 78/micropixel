// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdlib>
#include <span>
#include <string_view>
#include <unordered_set>

#include "platform/memory/psram_allocator.hpp"

namespace {
std::unordered_set<void*> allocations;
}  // namespace

void* micropixel_test_psram_allocate(size_t size) {
    void* memory = std::malloc(size);
    if (memory != nullptr) {
        allocations.insert(memory);
    }
    return memory;
}

void micropixel_test_psram_free(void* memory) {
    if (memory == nullptr) {
        return;
    }
    assert(allocations.erase(memory) == 1);
    std::free(memory);
}

namespace memory = micropixel::platform::memory;

void StringAndVectorExposeNonOwningViews() {
    {
        memory::PsramString text(64U, 'a');
        assert(allocations.size() >= 1U);
        const std::string_view view = text;
        assert(view.size() == 64U);
        assert(view.front() == 'a');
        assert(view.back() == 'a');
    }
    assert(allocations.empty());

    {
        memory::PsramVector<int> values;
        values.push_back(3);
        values.push_back(5);
        const std::span<const int> view = values;
        assert(view.size() == 2U);
        assert(view[0] == 3);
        assert(view[1] == 5);
        assert(!allocations.empty());
    }
    assert(allocations.empty());
}

void MapStoresPairsAndAllocatorComparesEqual() {
    {
        memory::PsramMap<int, int> items;
        items.emplace(1, 2);
        items.emplace(3, 4);
        assert(items.at(1) == 2);
        assert(items.at(3) == 4);
        assert(!allocations.empty());
    }
    assert(allocations.empty());
    assert(memory::PsramAllocator<int>{} == memory::PsramAllocator<char>{});
}

int main() {
    StringAndVectorExposeNonOwningViews();
    MapStoresPairsAndAllocatorComparesEqual();
    return 0;
}
