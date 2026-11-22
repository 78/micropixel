// SPDX-License-Identifier: Apache-2.0
#include "sd_app_storage.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "bflb_mtimer.h"
#include "sd_storage.h"

namespace mpx_port {
using Error = micropixel::device::BlockStorageError;

bool SdAppStorage::Fail(const char* error) {
    error_ = error;
    healthy_ = false;
    return false;
}

bool SdAppStorage::InRange(uint64_t offset, uint64_t bytes) const {
    return offset <= geometry_.size_bytes && bytes <= geometry_.size_bytes - offset;
}

bool SdAppStorage::Seek(uint64_t offset) {
    return f_lseek(&file_, static_cast<FSIZE_t>(offset)) == FR_OK && f_tell(&file_) == offset;
}

void SdAppStorage::EnableFastSeek() {
#if FF_USE_FASTSEEK
    link_map_[0] = sizeof(link_map_) / sizeof(link_map_[0]);
    file_.cltbl = link_map_;
    if (f_lseek(&file_, CREATE_LINKMAP) != FR_OK) file_.cltbl = nullptr;
#endif
}

bool SdAppStorage::Initialize(bool create, bool reformat_pending) {
    std::lock_guard lock(mutex_);
    if (opened_) return healthy_;
    if (!sd_storage_mount()) return Fail("card_unavailable");
    FILINFO info{};
    FRESULT status = f_stat(kPath, &info);
    if (status == FR_OK) {
        if ((info.fattrib & AM_DIR) || info.fsize != kImageBytes) return Fail("foreign_image_preserved");
        if (f_open(&file_, kPath, FA_READ | FA_WRITE) != FR_OK) return Fail("image_open_failed");
        opened_ = healthy_ = true;
        pending_ = false;
    } else {
        if (status != FR_NO_FILE && status != FR_NO_PATH) return Fail("image_stat_failed");
        if (!create) return Fail("image_absent");
        status = f_mkdir("/sd/micropixel");
        if (status != FR_OK && status != FR_EXIST) return Fail("directory_create_failed");
        // CREATE_NEW never truncates a previous initialization or another file.
        status = f_open(&file_, kPendingPath, FA_READ | FA_WRITE | FA_CREATE_NEW);
        if (status == FR_EXIST && reformat_pending) {
            // Explicit maintenance only. Never reached by startup/normal INIT.
            if (f_stat(kPendingPath, &info) != FR_OK || (info.fattrib & AM_DIR) || info.fsize != kImageBytes)
                return Fail("foreign_pending_preserved");
            if (f_open(&file_, kPendingPath, FA_READ | FA_WRITE) != FR_OK) return Fail("pending_open_failed");
            opened_ = healthy_ = pending_ = true;
        } else if (status != FR_OK) {
            printf("[sd-appstore] create FRESULT=%u\r\n", static_cast<unsigned>(status));
            return Fail("pending_file_exists_or_create_failed");
        }
        if (!opened_) {
            opened_ = healthy_ = pending_ = true;
#if FF_USE_EXPAND
            status = f_expand(&file_, kImageBytes, 1);
            if (status != FR_OK && status != FR_DENIED) return Fail("image_reserve_failed");
#endif
            // f_lseek allocates clusters when contiguous preallocation is unavailable.
            // Their initial contents are unspecified. Format writes only the new
            // metadata area, and BundleFS erases every data block before allocating it.
            if (!Seek(kImageBytes) || f_size(&file_) != kImageBytes || f_sync(&file_) != FR_OK)
                return Fail("image_reserve_failed");
        }
    }
    geometry_ = {kImageBytes, 4096, 1, false, 0};
    statistics_ = {};
    EnableFastSeek();
    error_ = pending_ ? "new_image" : "ready";
    return true;
}

bool SdAppStorage::Publish() {
    std::lock_guard lock(mutex_);
    if (!opened_ || !healthy_) return false;
    if (!pending_) return true;
    if (f_sync(&file_) != FR_OK) return Fail("image_sync_failed");
    const FRESULT closed = f_close(&file_);
    opened_ = false;
    if (closed != FR_OK || f_rename(kPendingPath, kPath) != FR_OK) return Fail("image_publish_failed");
    pending_ = false;
    if (f_open(&file_, kPath, FA_READ | FA_WRITE) != FR_OK) return Fail("image_reopen_failed");
    opened_ = true;
    EnableFastSeek();
    error_ = "ready";
    return true;
}

bool SdAppStorage::Close() {
    std::lock_guard lock(mutex_);
    bool good = true;
    if (opened_) {
        good = f_sync(&file_) == FR_OK;
        if (f_close(&file_) != FR_OK) good = false;
    }
    opened_ = healthy_ = pending_ = false;
    geometry_ = {};
    error_ = good ? "closed" : "close_failed";
    return good;
}

SdAppStorage::Statistics SdAppStorage::statistics() {
    std::lock_guard lock(mutex_);
    return statistics_;
}

std::expected<void, Error> SdAppStorage::Read(uint64_t offset, std::span<uint8_t> destination) {
    std::lock_guard lock(mutex_);
    if (!opened_ || !healthy_) return std::unexpected(Error::kUnavailable);
    if (!InRange(offset, destination.size())) return std::unexpected(Error::kInvalidArgument);
    if (destination.empty()) return {};
    const uint64_t start = bflb_mtimer_get_time_us();
    if (!Seek(offset)) {
        Fail("read_failed");
        return std::unexpected(Error::kIo);
    }
    // SDH DMA must not operate on arbitrary PSRAM/Guest buffers. Keep transfers
    // in our cache-line-aligned SRAM buffer and copy to the caller on the CPU.
    for (size_t done = 0; done < destination.size();) {
        const UINT count = std::min<size_t>(sizeof(scratch_), destination.size() - done);
        UINT received{};
        if (f_read(&file_, scratch_, count, &received) != FR_OK || received != count) {
            Fail("read_failed");
            return std::unexpected(Error::kIo);
        }
        std::memcpy(destination.data() + done, scratch_, count);
        done += count;
    }
    statistics_.read_us += bflb_mtimer_get_time_us() - start;
    statistics_.read_bytes += destination.size();
    ++statistics_.read_calls;
    return {};
}

std::expected<void, Error> SdAppStorage::Program(uint64_t offset, std::span<const uint8_t> source) {
    std::lock_guard lock(mutex_);
    if (!opened_ || !healthy_) return std::unexpected(Error::kUnavailable);
    if (!InRange(offset, source.size())) return std::unexpected(Error::kInvalidArgument);
    for (size_t done = 0; done < source.size();) {
        const UINT count = std::min<size_t>(sizeof(scratch_), source.size() - done);
        UINT transferred{};
        if (!Seek(offset + done) || f_read(&file_, scratch_, count, &transferred) != FR_OK || transferred != count) {
            Fail("program_read_failed");
            return std::unexpected(Error::kIo);
        }
        // Emulate NOR programming, including four-byte commit marker updates.
        for (UINT i = 0; i < count; ++i)
            if ((scratch_[i] & source[done + i]) != source[done + i]) return std::unexpected(Error::kInvalidArgument);
        std::memcpy(scratch_, source.data() + done, count);
        if (!Seek(offset + done) || f_write(&file_, scratch_, count, &transferred) != FR_OK || transferred != count) {
            Fail("program_write_failed");
            return std::unexpected(Error::kIo);
        }
        statistics_.write_bytes += count;
        done += count;
    }
    // Persist the body before BundleFS programs its separate commit marker.
    if (f_sync(&file_) != FR_OK) {
        Fail("program_sync_failed");
        return std::unexpected(Error::kIo);
    }
    return {};
}

std::expected<void, Error> SdAppStorage::Erase(uint64_t offset, uint64_t bytes) {
    std::lock_guard lock(mutex_);
    if (!opened_ || !healthy_) return std::unexpected(Error::kUnavailable);
    if (offset % 4096 || bytes % 4096 || !InRange(offset, bytes)) return std::unexpected(Error::kInvalidArgument);
    if (!Seek(offset)) {
        Fail("erase_seek_failed");
        return std::unexpected(Error::kIo);
    }
    std::memset(scratch_, 255, sizeof(scratch_));
    while (bytes) {
        UINT written{};
        if (f_write(&file_, scratch_, sizeof(scratch_), &written) != FR_OK || written != sizeof(scratch_)) {
            Fail("erase_failed");
            return std::unexpected(Error::kIo);
        }
        statistics_.write_bytes += written;
        bytes -= written;
    }
    if (f_sync(&file_) != FR_OK) {
        Fail("erase_sync_failed");
        return std::unexpected(Error::kIo);
    }
    return {};
}

std::expected<void, Error> SdAppStorage::Sync() {
    std::lock_guard lock(mutex_);
    if (!opened_ || !healthy_) return std::unexpected(Error::kUnavailable);
    if (f_sync(&file_) != FR_OK) {
        Fail("sync_failed");
        return std::unexpected(Error::kIo);
    }
    return {};
}
}  // namespace mpx_port
