// SPDX-License-Identifier: Apache-2.0
#include "platform/storage/sector_block_storage.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace micropixel::platform::storage {
namespace {

constexpr uint8_t kErasedByte = 0xFFU;

}  // namespace

SectorBlockStorage::SectorBlockStorage(SectorDevice& device, std::span<uint8_t> workspace)
    : device_(device), workspace_(workspace) {
    sector_size_ = device_.sector_size();
    // The sector is the erase unit. Programs are advertised as byte granular so
    // BundleFS can land a lone four-byte commit marker; a card only takes whole
    // sectors, so Program() merges those writes itself. BundleFS starts its own
    // data blocks from this erase unit and doubles while its Catalog block map
    // would exceed the configured RAM budget, so every offset it erases stays a
    // multiple of the sector.
    geometry_ = {
        .size_bytes = sector_size_ != 0U ? device_.sector_count() * sector_size_ : 0U,
        .erase_size = sector_size_,
        .program_size = 1U,
        .mappable = false,
        .map_alignment = 0U,
    };
}

size_t SectorBlockStorage::WorkspaceSectors() const { return workspace_.size() / sector_size_; }

bool SectorBlockStorage::InRange(uint64_t offset, uint64_t size) const {
    return offset <= geometry_.size_bytes && size <= geometry_.size_bytes - offset;
}

std::expected<void, device::BlockStorageError> SectorBlockStorage::ReadSectors(uint64_t first_sector, size_t count,
                                                                               uint8_t* destination) {
    if (count == 0U) {
        return {};
    }
    if (!device_.ReadSectors(first_sector, count, destination)) {
        return std::unexpected(device::BlockStorageError::kIo);
    }
    return {};
}

std::expected<void, device::BlockStorageError> SectorBlockStorage::WriteSectors(uint64_t first_sector, size_t count,
                                                                                const uint8_t* source) {
    if (count == 0U) {
        return {};
    }
    if (!device_.WriteSectors(first_sector, count, source)) {
        return std::unexpected(device::BlockStorageError::kIo);
    }
    return {};
}

std::expected<void, device::BlockStorageError> SectorBlockStorage::Read(uint64_t offset,
                                                                        std::span<uint8_t> destination) {
    const uint32_t sector_size = sector_size_;
    if (sector_size == 0U || workspace_.size() < sector_size) {
        return std::unexpected(device::BlockStorageError::kUnavailable);
    }
    if (!InRange(offset, destination.size())) {
        return std::unexpected(device::BlockStorageError::kInvalidArgument);
    }
    // Aligned reads go straight into the caller's buffer.
    if ((offset % sector_size) == 0U && (destination.size() % sector_size) == 0U) {
        return ReadSectors(offset / sector_size, destination.size() / sector_size, destination.data());
    }
    const size_t window_sectors = WorkspaceSectors();
    const size_t window_size = window_sectors * sector_size;
    size_t consumed = 0U;
    while (consumed < destination.size()) {
        const uint64_t position = offset + consumed;
        const uint64_t first_sector = position / sector_size;
        const size_t head = static_cast<size_t>(position % sector_size);
        const size_t wanted = std::min(destination.size() - consumed, window_size - head);
        const size_t sectors = (head + wanted + sector_size - 1U) / sector_size;
        const auto read_status = ReadSectors(first_sector, sectors, workspace_.data());
        if (!read_status) {
            return read_status;
        }
        std::memcpy(destination.data() + consumed, workspace_.data() + head, wanted);
        consumed += wanted;
    }
    return {};
}

std::expected<void, device::BlockStorageError> SectorBlockStorage::Program(uint64_t offset,
                                                                           std::span<const uint8_t> source) {
    if (sector_size_ == 0U || workspace_.size() < sector_size_) {
        return std::unexpected(device::BlockStorageError::kUnavailable);
    }
    if (!InRange(offset, source.size())) {
        return std::unexpected(device::BlockStorageError::kInvalidArgument);
    }
    size_t consumed = 0U;
    while (consumed < source.size()) {
        const uint64_t position = offset + consumed;
        const uint64_t first_sector = position / sector_size_;
        const size_t head = static_cast<size_t>(position % sector_size_);
        const size_t chunk = std::min<size_t>(sector_size_ - head, source.size() - consumed);
        if (head == 0U && chunk == sector_size_) {
            const auto write_status = WriteSectors(first_sector, 1U, source.data() + consumed);
            if (!write_status) {
                return write_status;
            }
        } else {
            // Partial sector: merge into the current contents so an earlier
            // chunk (a record body next to its commit marker) survives.
            const auto read_status = ReadSectors(first_sector, 1U, workspace_.data());
            if (!read_status) {
                return read_status;
            }
            std::memcpy(workspace_.data() + head, source.data() + consumed, chunk);
            const auto write_status = WriteSectors(first_sector, 1U, workspace_.data());
            if (!write_status) {
                return write_status;
            }
        }
        consumed += chunk;
    }
    return {};
}

std::expected<void, device::BlockStorageError> SectorBlockStorage::Erase(uint64_t offset, uint64_t size) {
    if (sector_size_ == 0U || workspace_.size() < sector_size_) {
        return std::unexpected(device::BlockStorageError::kUnavailable);
    }
    if ((offset % geometry_.erase_size) != 0U || (size % geometry_.erase_size) != 0U) {
        return std::unexpected(device::BlockStorageError::kInvalidArgument);
    }
    if (!InRange(offset, size)) {
        return std::unexpected(device::BlockStorageError::kInvalidArgument);
    }
    const size_t window_sectors = WorkspaceSectors();
    const size_t window_size = window_sectors * sector_size_;
    std::memset(workspace_.data(), kErasedByte, window_size);
    uint64_t remaining = size;
    uint64_t position = offset;
    while (remaining > 0U) {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, window_size));
        const auto write_status = WriteSectors(position / sector_size_, chunk / sector_size_, workspace_.data());
        if (!write_status) {
            return write_status;
        }
        position += chunk;
        remaining -= chunk;
    }
    return {};
}

std::expected<device::BlockStorageMapping, device::BlockStorageError> SectorBlockStorage::Map(
    std::span<const uint64_t> /*block_offsets*/, uint32_t /*block_size*/) {
    return std::unexpected(device::BlockStorageError::kUnsupported);
}

void SectorBlockStorage::Unmap(device::BlockStorageMapping& /*mapping*/) {}

}  // namespace micropixel::platform::storage
