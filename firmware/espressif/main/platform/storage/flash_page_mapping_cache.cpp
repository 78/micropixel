// SPDX-License-Identifier: Apache-2.0
#include "platform/storage/flash_page_mapping_cache.hpp"

#include <algorithm>
#include <cstring>

#include "esp_log.h"
#include "spi_flash_mmap.h"

namespace micropixel::platform::storage {
namespace {
constexpr char kTag[] = "flash_page_cache";
}

void FlashPageMappingCache::ReleaseWindow(Window& window) {
    if (window.data != nullptr) {
        spi_flash_munmap(window.backend_handle);
    }
    heap_caps_free(window.pages);
    window = {};
}

FlashPageMappingCache::~FlashPageMappingCache() {
    if (state_.size() != 0U) {
        for (auto& window : state_.View()[0].windows) {
            ReleaseWindow(window);
        }
    }
}

std::expected<device::BlockStorageMapping, device::BlockStorageError> FlashPageMappingCache::Map(
    std::span<const int> pages) {
    if (pages.empty() || pages.size() > kMaxPages ||
        std::any_of(pages.begin(), pages.end(), [](int page) { return page < 0; })) {
        return std::unexpected(device::BlockStorageError::kInvalidArgument);
    }
    const std::lock_guard lock(mutex_);
    if (next_handle_ == 0U || (state_.size() == 0U && !state_.Allocate(1U))) {
        return std::unexpected(device::BlockStorageError::kUnavailable);
    }
    auto& state = state_.View()[0];
    auto lease =
        std::find_if(state.leases.begin(), state.leases.end(), [](const Lease& item) { return item.handle == 0U; });
    if (lease == state.leases.end()) {
        return std::unexpected(device::BlockStorageError::kUnavailable);
    }
    size_t selected = kWindowCapacity;
    for (size_t index = 0U; index < state.windows.size(); ++index) {
        const auto& window = state.windows[index];
        if (window.references != 0U && pages.size() == window.page_count &&
            std::equal(pages.begin(), pages.end(), window.pages)) {
            selected = index;
            break;
        }
    }
    if (selected == kWindowCapacity) {
        for (size_t index = 0U; index < state.windows.size(); ++index) {
            if (state.windows[index].references == 0U) {
                selected = index;
                break;
            }
        }
        if (selected == kWindowCapacity) {
            return std::unexpected(device::BlockStorageError::kUnavailable);
        }
        auto& window = state.windows[selected];
        window.pages = static_cast<int*>(heap_caps_malloc(pages.size_bytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (window.pages == nullptr) {
            return std::unexpected(device::BlockStorageError::kUnavailable);
        }
        std::memcpy(window.pages, pages.data(), pages.size_bytes());
        spi_flash_mmap_handle_t backend_handle{};
        const void* data = nullptr;
        const esp_err_t error =
            spi_flash_mmap_pages(pages.data(), pages.size(), SPI_FLASH_MMAP_FLAG_DATA, &data, &backend_handle);
        if (error != ESP_OK) {
            ReleaseWindow(window);
            ESP_LOGW(kTag, "page window unavailable: pages=%u error=%s", static_cast<unsigned>(pages.size()),
                     esp_err_to_name(error));
            return std::unexpected(device::BlockStorageError::kUnavailable);
        }
        window.data = data;
        window.backend_handle = backend_handle;
        window.page_count = pages.size();
    }
    auto& window = state.windows[selected];
    const void* data = window.data;
    *lease = Lease{.data = data, .handle = next_handle_++, .window_index = selected};
    ++window.references;
    return device::BlockStorageMapping{.data = data, .handle = lease->handle};
}

void FlashPageMappingCache::Unmap(device::BlockStorageMapping& mapping) {
    const std::lock_guard lock(mutex_);
    if (mapping.handle != 0U && state_.size() != 0U) {
        auto& state = state_.View()[0];
        for (auto& lease : state.leases) {
            if (lease.handle == mapping.handle && lease.data == mapping.data) {
                auto& window = state.windows[lease.window_index];
                lease = {};
                if (--window.references == 0U) {
                    ReleaseWindow(window);
                }
                break;
            }
        }
    }
    mapping = {};
}

}  // namespace micropixel::platform::storage
