#include "platform/storage/partition_block_storage.hpp"

#include "esp_heap_caps.h"
#include "spi_flash_mmap.h"

namespace micropixel::platform::storage {
namespace {

constexpr uint32_t kEraseSectorSize = 4096U;

device::BlockStorageGeometry GeometryOf(const esp_partition_t* partition) {
    if (partition == nullptr) {
        return {};
    }
    return device::BlockStorageGeometry{
        .size_bytes = partition->size,
        .erase_size = kEraseSectorSize,
        .program_size = 1U,
        .mappable = true,
        .map_alignment = SPI_FLASH_MMU_PAGE_SIZE,
    };
}

}  // namespace

PartitionBlockStorage::PartitionBlockStorage(const char* label, esp_partition_subtype_t subtype)
    : PartitionBlockStorage(esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, label)) {}

PartitionBlockStorage::PartitionBlockStorage(const esp_partition_t* partition)
    : partition_(partition), geometry_(GeometryOf(partition)) {}

bool PartitionBlockStorage::InRange(uint64_t offset, uint64_t size) const {
    return partition_ != nullptr && offset <= partition_->size && size <= partition_->size - offset;
}

std::expected<void, device::BlockStorageError> PartitionBlockStorage::Read(uint64_t offset,
                                                                           std::span<uint8_t> destination) {
    if (!InRange(offset, destination.size())) {
        return std::unexpected(partition_ == nullptr ? device::BlockStorageError::kUnavailable
                                                     : device::BlockStorageError::kInvalidArgument);
    }
    if (esp_partition_read(partition_, static_cast<size_t>(offset), destination.data(), destination.size()) != ESP_OK) {
        return std::unexpected(device::BlockStorageError::kIo);
    }
    return {};
}

std::expected<void, device::BlockStorageError> PartitionBlockStorage::Program(uint64_t offset,
                                                                              std::span<const uint8_t> source) {
    if (!InRange(offset, source.size())) {
        return std::unexpected(partition_ == nullptr ? device::BlockStorageError::kUnavailable
                                                     : device::BlockStorageError::kInvalidArgument);
    }
    if (esp_partition_write(partition_, static_cast<size_t>(offset), source.data(), source.size()) != ESP_OK) {
        return std::unexpected(device::BlockStorageError::kIo);
    }
    return {};
}

std::expected<void, device::BlockStorageError> PartitionBlockStorage::Erase(uint64_t offset, uint64_t size) {
    if (!InRange(offset, size) || (offset % kEraseSectorSize) != 0U || (size % kEraseSectorSize) != 0U) {
        return std::unexpected(partition_ == nullptr ? device::BlockStorageError::kUnavailable
                                                     : device::BlockStorageError::kInvalidArgument);
    }
    if (esp_partition_erase_range(partition_, static_cast<size_t>(offset), static_cast<size_t>(size)) != ESP_OK) {
        return std::unexpected(device::BlockStorageError::kIo);
    }
    return {};
}

std::expected<device::BlockStorageMapping, device::BlockStorageError> PartitionBlockStorage::Map(
    std::span<const uint64_t> block_offsets, uint32_t block_size) {
    if (partition_ == nullptr) {
        return std::unexpected(device::BlockStorageError::kUnavailable);
    }
    if (block_offsets.empty() || block_size == 0U || (block_size % SPI_FLASH_MMU_PAGE_SIZE) != 0U) {
        return std::unexpected(device::BlockStorageError::kInvalidArgument);
    }
    const uint32_t pages_per_block = block_size / SPI_FLASH_MMU_PAGE_SIZE;
    if (block_offsets.size() > FlashPageMappingCache::kMaxPages / pages_per_block) {
        return std::unexpected(device::BlockStorageError::kUnavailable);
    }
    const uint32_t page_count = static_cast<uint32_t>(block_offsets.size()) * pages_per_block;
    int* pages =
        static_cast<int*>(heap_caps_malloc(page_count * sizeof(*pages), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (pages == nullptr) {
        return std::unexpected(device::BlockStorageError::kUnavailable);
    }
    for (uint32_t block_index = 0U; block_index < block_offsets.size(); ++block_index) {
        if (!InRange(block_offsets[block_index], block_size)) {
            heap_caps_free(pages);
            return std::unexpected(device::BlockStorageError::kInvalidArgument);
        }
        const uint32_t block_physical = partition_->address + static_cast<uint32_t>(block_offsets[block_index]);
        if ((block_physical % SPI_FLASH_MMU_PAGE_SIZE) != 0U) {
            heap_caps_free(pages);
            return std::unexpected(device::BlockStorageError::kInvalidArgument);
        }
        for (uint32_t page_index = 0U; page_index < pages_per_block; ++page_index) {
            pages[block_index * pages_per_block + page_index] =
                static_cast<int>(block_physical / SPI_FLASH_MMU_PAGE_SIZE + page_index);
        }
    }
    auto mapping = mapping_cache_.Map(std::span<const int>(pages, page_count));
    heap_caps_free(pages);
    return mapping;
}

void PartitionBlockStorage::Unmap(device::BlockStorageMapping& mapping) { mapping_cache_.Unmap(mapping); }

}  // namespace micropixel::platform::storage
