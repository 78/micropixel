// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "device/contracts/block_storage.hpp"
extern "C" {
#include "bflb_mtd.h"
}
namespace mpx_port {
class FlashStorage final : public micropixel::device::BlockStorage {
    bflb_mtd_handle_t handle_{};
    micropixel::device::BlockStorageGeometry geometry_{};

   public:
    ~FlashStorage() override;
    bool Initialize();
    const micropixel::device::BlockStorageGeometry& geometry() const override { return geometry_; }
    std::expected<void, micropixel::device::BlockStorageError> Read(uint64_t, std::span<uint8_t>) override;
    std::expected<void, micropixel::device::BlockStorageError> Program(uint64_t, std::span<const uint8_t>) override;
    std::expected<void, micropixel::device::BlockStorageError> Erase(uint64_t, uint64_t) override;
    std::expected<micropixel::device::BlockStorageMapping, micropixel::device::BlockStorageError> Map(
        std::span<const uint64_t>, uint32_t) override {
        return std::unexpected(micropixel::device::BlockStorageError::kUnsupported);
    }
    void Unmap(micropixel::device::BlockStorageMapping& m) override { m = {}; }
};
bool InitializeKv();
}  // namespace mpx_port
