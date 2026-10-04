// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/m5stack-tab5/sd_card.hpp"

#include <algorithm>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "platform/boards/m5stack-tab5/board_config.hpp"
#include "sdmmc_cmd.h"

namespace micropixel::platform::m5stack_tab5 {
namespace {

constexpr char kTag[] = "tab5_sdcard";
// Working buffer for erase fills and unaligned reads: eight 4 KiB chunks keep
// the command count low without holding a large block in internal RAM.
constexpr size_t kWorkspaceBytes = 32U * 1024U;
constexpr uint32_t kKilobytesPerMegabyte = 1024U;

}  // namespace

SdCard::~SdCard() {
    if (power_ != nullptr) {
        (void)sd_pwr_ctrl_del_on_chip_ldo(power_);
    }
}

uint32_t SdCard::SectorLink::sector_size() const {
    return card_ != nullptr ? static_cast<uint32_t>(card_->csd.sector_size) : 0U;
}

uint64_t SdCard::SectorLink::sector_count() const {
    return card_ != nullptr ? static_cast<uint64_t>(card_->csd.capacity) : 0U;
}

bool SdCard::SectorLink::ReadSectors(uint64_t first_sector, size_t count, uint8_t* destination) {
    return card_ != nullptr &&
           sdmmc_read_sectors(card_, destination, static_cast<size_t>(first_sector), count) == ESP_OK;
}

bool SdCard::SectorLink::WriteSectors(uint64_t first_sector, size_t count, const uint8_t* source) {
    return card_ != nullptr && sdmmc_write_sectors(card_, source, static_cast<size_t>(first_sector), count) == ESP_OK;
}

void SdCard::Initialize() {
    // The card's IO rail comes from the P4's on-chip LDO (channel 4 = LDO_VO4 on
    // this board, the same rail the vendor BSP uses) and must be up before the
    // bus is touched.
    sd_pwr_ctrl_ldo_config_t ldo_config{};
    ldo_config.ldo_chan_id = board::kSdCardLdoChannel;
    const esp_err_t power_status = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &power_);
    if (power_status != ESP_OK) {
        power_ = nullptr;
        ESP_LOGW(kTag, "card rail unavailable: %s; Apps stay in the NOR app_store", esp_err_to_name(power_status));
        return;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = power_;
    const esp_err_t host_status = sdmmc_host_init();
    if (host_status != ESP_OK) {
        ESP_LOGW(kTag, "SDMMC host unavailable: %s; Apps stay in the NOR app_store", esp_err_to_name(host_status));
        return;
    }
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = board::kSdCardBusWidth;
    slot.clk = board::kSdCardClock;
    slot.cmd = board::kSdCardCommand;
    slot.d0 = board::kSdCardData0;
    slot.d1 = board::kSdCardData1;
    slot.d2 = board::kSdCardData2;
    slot.d3 = board::kSdCardData3;
    const esp_err_t slot_status = sdmmc_host_init_slot(host.slot, &slot);
    if (slot_status != ESP_OK) {
        ESP_LOGW(kTag, "SDMMC slot unavailable: %s; Apps stay in the NOR app_store", esp_err_to_name(slot_status));
        return;
    }
    host_ready_ = true;
    const esp_err_t card_status = sdmmc_card_init(&host, &card_);
    if (card_status != ESP_OK) {
        ESP_LOGI(kTag, "no card in the socket (%s); Apps stay in the NOR app_store", esp_err_to_name(card_status));
        return;
    }
    if (!workspace_.Allocate(kWorkspaceBytes)) {
        ESP_LOGW(kTag, "no PSRAM workspace for the card; Apps stay in the NOR app_store");
        return;
    }
    link_.Bind(&card_);
    storage_.emplace(link_, workspace_.View());
    const uint64_t size_bytes = storage_->geometry().size_bytes;
    // The App Store reports the BundleFS data block it derived from this
    // geometry; the board only owns the medium itself.
    ESP_LOGI(kTag, "ready: %s, %llu MiB, %u byte sectors", card_.cid.name,
             static_cast<unsigned long long>(size_bytes / (kKilobytesPerMegabyte * kKilobytesPerMegabyte)),
             static_cast<unsigned>(storage_->geometry().erase_size));
}

}  // namespace micropixel::platform::m5stack_tab5
