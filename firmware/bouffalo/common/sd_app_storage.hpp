// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "device/contracts/block_storage.hpp"
#include "micropixel_mutex.hpp"
extern "C" {
#include "ff.h"
}

namespace mpx_port {
// A private BundleFS image inside an existing FAT/exFAT card. Only an explicitly
// created temporary file may be formatted; foreign/existing images are preserved.
class SdAppStorage final : public micropixel::device::BlockStorage {
   public:
    static constexpr uint32_t kImageBytes = 64U * 1024U * 1024U;
    static constexpr const char* kPath = "/sd/micropixel/apps.bfs";
    static constexpr const char* kPendingPath = "/sd/micropixel/apps.new";
    struct Statistics {
        uint64_t read_bytes{}, read_us{}, write_bytes{};
        uint32_t read_calls{};
    };
    bool Initialize(bool create, bool reformat_pending = false);
    bool Publish();
    bool Close();
    bool pending() const { return pending_; }
    bool available() const { return opened_ && healthy_; }
    const char* error() const { return error_; }
    Statistics statistics();
    const micropixel::device::BlockStorageGeometry& geometry() const override { return geometry_; }
    std::expected<void, micropixel::device::BlockStorageError> Read(uint64_t, std::span<uint8_t>) override;
    std::expected<void, micropixel::device::BlockStorageError> Program(uint64_t, std::span<const uint8_t>) override;
    std::expected<void, micropixel::device::BlockStorageError> Erase(uint64_t, uint64_t) override;
    std::expected<void, micropixel::device::BlockStorageError> Sync() override;
    std::expected<micropixel::device::BlockStorageMapping, micropixel::device::BlockStorageError> Map(
        std::span<const uint64_t>, uint32_t) override {
        return std::unexpected(micropixel::device::BlockStorageError::kUnsupported);
    }
    void Unmap(micropixel::device::BlockStorageMapping& mapping) override { mapping = {}; }

   private:
    bool Fail(const char* error);
    bool Seek(uint64_t);
    void EnableFastSeek();
    bool InRange(uint64_t offset, uint64_t bytes) const;
    BouffaloMutex mutex_;
    FIL file_{};
    micropixel::device::BlockStorageGeometry geometry_{};
    alignas(64) uint8_t scratch_[4096]{};
    DWORD link_map_[128]{};
    Statistics statistics_{};
    bool opened_{}, healthy_{}, pending_{};
    const char* error_{"closed"};
};
}  // namespace mpx_port
