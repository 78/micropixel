// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "device/contracts/block_storage.hpp"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "platform/memory/psram_buffer.hpp"
#include "platform/storage/sector_block_storage.hpp"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

namespace micropixel::platform::m5stack_tab5 {

// microSD socket as the board's external App storage medium. The card holds a
// BundleFS (never a filesystem): the App Store mounts it, downloaded Apps land
// there and the NOR app_store partition keeps Components and factory Apps.
// Because the medium is the user's, the platform never formats it on its own -
// a foreign card shows up as "needs formatting" and the System settings ask
// first. There is no card-detect pin, so insertion is only noticed at boot.
class SdCard final {
   public:
    SdCard() = default;
    SdCard(const SdCard&) = delete;
    SdCard& operator=(const SdCard&) = delete;
    ~SdCard();

    // Powers the card rail from the P4's on-chip LDO, brings up SDMMC slot 0
    // and identifies the card. Without a card (or when identification fails)
    // storage() stays null and the board keeps installing every App into NOR.
    void Initialize();
    [[nodiscard]] device::BlockStorage* storage() {
        return storage_.has_value() ? &*storage_ : nullptr;  // NOLINT(readability-identifier-naming)
    }

   private:
    class SectorLink final : public storage::SectorDevice {
       public:
        void Bind(sdmmc_card_t* card) { card_ = card; }
        [[nodiscard]] uint32_t sector_size() const override;
        [[nodiscard]] uint64_t sector_count() const override;
        [[nodiscard]] bool ReadSectors(uint64_t first_sector, size_t count, uint8_t* destination) override;
        [[nodiscard]] bool WriteSectors(uint64_t first_sector, size_t count, const uint8_t* source) override;

       private:
        sdmmc_card_t* card_{};
    };

    sdmmc_card_t card_{};
    sd_pwr_ctrl_handle_t power_{};
    bool host_ready_{};
    SectorLink link_{};
    memory::PsramBuffer<uint8_t> workspace_{};
    std::optional<storage::SectorBlockStorage> storage_{};
};

}  // namespace micropixel::platform::m5stack_tab5
