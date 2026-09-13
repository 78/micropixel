// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "platform/storage/flash_page_mapping_cache.hpp"
#include "spi_flash_mmap.h"

using micropixel::device::BlockStorageMapping;
using micropixel::platform::storage::FlashPageMappingCache;

namespace {
struct BackendWindow {
    std::vector<int> pages;
    void* data{};
    uint32_t handle{};
};
std::vector<BackendWindow> backend;
uint32_t next_backend_handle = 1U;
uint32_t map_calls{};
uint32_t unmap_calls{};
int allocations_before_failure = -1;
size_t live_allocations{};
bool fail_backend{};

void Check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::abort();
    }
}

void TestSharing() {
    FlashPageMappingCache cache;
    const std::array pages{10, 20, 30};
    auto first = cache.Map(pages);
    Check(first.has_value(), "initial scattered window");
    const auto calls = map_calls;
    auto second = cache.Map(pages);
    auto middle = cache.Map(pages);
    auto suffix = cache.Map(pages);
    Check(second && middle && suffix, "whole-file leases share one mapping");
    Check(map_calls == calls, "sharing must not call ESP-IDF again");
    Check(first->handle != second->handle && second->handle != middle->handle, "independent lease handles");
    Check(middle->data == first->data, "whole-file base address is shared");
    Check(suffix->data == middle->data, "all leases start at the file base");
    Check(static_cast<const uint8_t*>(suffix->data)[2U * SPI_FLASH_MMU_PAGE_SIZE] == 30U, "page ordering preserved");
    auto stale = *first;
    cache.Unmap(*first);
    cache.Unmap(stale);
    Check(backend.size() == 1U, "duplicate release must not consume another lease");
    auto wrong_address = *second;
    wrong_address.data = nullptr;
    cache.Unmap(wrong_address);
    cache.Unmap(*middle);
    cache.Unmap(*suffix);
    Check(backend.size() == 1U, "last live lease keeps backend mapped");
    cache.Unmap(*second);
    Check(backend.empty(), "last release unmaps backend");
    auto replacement = cache.Map(pages);
    Check(replacement && replacement->handle != stale.handle, "slot reuse gets a fresh handle");
    // Reconstruct the original stale lease after its first no-op release cleared it.
    stale = BlockStorageMapping{.data = replacement->data, .handle = 1U};
    cache.Unmap(stale);
    Check(backend.size() == 1U, "old token cannot release a reused slot even at the same address");
    cache.Unmap(*replacement);
}

void TestFailures() {
    FlashPageMappingCache cache;
    const std::array pages{100, 200};
    allocations_before_failure = 0;
    Check(!cache.Map(pages), "state allocation failure");
    allocations_before_failure = 1;
    Check(!cache.Map(pages), "page metadata allocation failure");
    allocations_before_failure = -1;
    auto held = cache.Map(pages);
    Check(held.has_value(), "retry after allocation failure");
    const std::array other{400};
    const auto live = live_allocations;
    fail_backend = true;
    Check(!cache.Map(other), "backend failure propagates");
    fail_backend = false;
    Check(live_allocations == live, "backend failure frees staged metadata");
    const std::array reversed{200, 100};
    Check(!cache.Map(reversed), "reordered physical pages cannot alias an existing window");
    const std::array overlapping{200, 300};
    Check(!cache.Map(overlapping), "unshareable overlap preserves backend failure for reader fallback");
    Check(static_cast<const uint8_t*>(held->data)[SPI_FLASH_MMU_PAGE_SIZE] == 200U, "failures preserve live window");
    auto recovered = cache.Map(other);
    Check(recovered.has_value(), "retry after backend failure");
    cache.Unmap(*recovered);
    cache.Unmap(*held);
    Check(!cache.Map({}), "empty page list rejected");
    const std::array negative{-1};
    Check(!cache.Map(negative), "negative physical page rejected");
    const std::vector<int> oversized(FlashPageMappingCache::kMaxPages + 1U, 1);
    Check(!cache.Map(oversized), "page count bound enforced");
}

void TestCapacityAndCleanup() {
    {
        FlashPageMappingCache cache;
        const std::array pages{500};
        std::array<BlockStorageMapping, FlashPageMappingCache::kLeaseCapacity> leases{};
        for (auto& lease : leases) {
            auto result = cache.Map(pages);
            Check(result.has_value(), "lease capacity available");
            lease = *result;
        }
        const auto calls = map_calls;
        Check(!cache.Map(pages), "lease pool exhaustion");
        Check(map_calls == calls && backend.size() == 1U, "lease exhaustion leaves backend intact");
        cache.Unmap(leases[0]);
        auto result = cache.Map(pages);
        Check(result.has_value(), "released lease slot reusable");
        // Destructor performs best-effort cleanup for outstanding leases.
    }
    Check(backend.empty(), "destructor releases shared backend once");
    {
        FlashPageMappingCache cache;
        for (size_t index = 0; index < FlashPageMappingCache::kWindowCapacity; ++index) {
            const std::array pages{static_cast<int>(1000U + index)};
            Check(cache.Map(pages).has_value(), "window capacity available");
        }
        const auto calls = map_calls;
        const std::array extra{2000};
        Check(!cache.Map(extra), "window pool exhaustion");
        Check(calls == map_calls, "window exhaustion does not allocate backend");
        const std::array existing{1000};
        Check(cache.Map(existing).has_value(), "full window pool still permits sharing");
    }
    Check(backend.empty(), "destructor releases all distinct windows");
}

void TestConcurrentLeases() {
    FlashPageMappingCache cache;
    const std::array pages{3000};
    auto held = cache.Map(pages);
    Check(held.has_value(), "concurrency base window");
    const auto calls = map_calls;
    std::array<std::thread, 8> workers;
    for (auto& worker : workers) {
        worker = std::thread([&] {
            for (size_t index = 0; index < 200; ++index) {
                auto lease = cache.Map(pages);
                Check(lease && lease->data == held->data, "concurrent acquire");
                cache.Unmap(*lease);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    Check(map_calls == calls && backend.size() == 1U, "concurrent borrowing keeps one backend");
    cache.Unmap(*held);
}
}  // namespace

void* micropixel_test_psram_allocate(size_t size) {
    if (allocations_before_failure == 0) {
        return nullptr;
    }
    if (allocations_before_failure > 0) {
        --allocations_before_failure;
    }
    void* result = std::malloc(size);
    if (result != nullptr) {
        ++live_allocations;
    }
    return result;
}

void micropixel_test_psram_free(void* pointer) {
    if (pointer != nullptr) {
        Check(live_allocations != 0U, "metadata free must have an owner");
        --live_allocations;
        std::free(pointer);
    }
}

esp_err_t spi_flash_mmap_pages(const int* pages, size_t count, uint32_t, const void** data,
                               spi_flash_mmap_handle_t* handle) {
    ++map_calls;
    if (fail_backend) {
        return ESP_FAIL;
    }
    // Model the IDF failure that motivated sharing: a scattered request has a
    // physical run already enclosed in a live backend mapping.
    for (const auto& window : backend) {
        for (size_t index = 0; index < count; ++index) {
            if (std::find(window.pages.begin(), window.pages.end(), pages[index]) != window.pages.end()) {
                return ESP_FAIL;
            }
        }
    }
    auto* bytes = static_cast<uint8_t*>(std::malloc(count * SPI_FLASH_MMU_PAGE_SIZE));
    Check(bytes != nullptr, "test backend allocation");
    for (size_t index = 0; index < count; ++index) {
        std::memset(bytes + index * SPI_FLASH_MMU_PAGE_SIZE, pages[index], SPI_FLASH_MMU_PAGE_SIZE);
    }
    *data = bytes;
    *handle = next_backend_handle++;
    backend.push_back(BackendWindow{.pages = {pages, pages + count}, .data = bytes, .handle = *handle});
    return ESP_OK;
}

void spi_flash_munmap(spi_flash_mmap_handle_t handle) {
    auto window = std::find_if(backend.begin(), backend.end(), [=](const auto& item) { return item.handle == handle; });
    Check(window != backend.end(), "backend must unmap exactly once");
    ++unmap_calls;
    std::free(window->data);
    backend.erase(window);
}

int main() {
    TestSharing();
    TestFailures();
    TestCapacityAndCleanup();
    TestConcurrentLeases();
    Check(backend.empty() && live_allocations == 0U, "all backend windows and PSRAM metadata released");
    std::printf(
        "Flash page cache: sharing, stale handles, OOM, rollback, capacities and concurrency passed (%u unmaps).\n",
        unmap_calls);
}
