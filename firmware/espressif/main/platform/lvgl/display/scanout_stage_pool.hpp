#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace micropixel::platform::lvgl {

// Fixed pool of full-frame RGB565 staging buffers shared by everything that
// scans out to the panel: the Direct Surface presenter (byte-swap/scale
// stages, overlay backup) and the board's system transition compositors
// (compose/wire stages, retained status-layer background and scrim).
//
// Those users are mutually exclusive in time; the ScanoutArbiter hands the
// panel to exactly one of them. Giving each its own permanently allocated
// frames therefore only wastes PSRAM (six frames on a 480x480 board where
// four ever coexist). Slots are allocated once at board initialization, so
// transitions never depend on a large heap allocation succeeding while a
// Guest holds most of PSRAM.
//
// Acquire/Release are non-blocking and callable from any task (presenter,
// LVGL or controller). A board that never initializes the pool leaves users
// on their private heap allocations.
class ScanoutStagePool final {
   public:
    static constexpr uint32_t kMaxSlots = 6U;
    // Address and size alignment of every slot; covers PPA/DMA2D descriptor
    // and cache line requirements on P4/S31.
    static constexpr uint32_t kSlotAlignment = 128U;

    static ScanoutStagePool& Instance();

    ScanoutStagePool(const ScanoutStagePool&) = delete;
    ScanoutStagePool& operator=(const ScanoutStagePool&) = delete;

    // Allocates slot_count PSRAM slots of at least slot_bytes each (rounded up
    // to kSlotAlignment). Only the first successful call has an effect.
    [[nodiscard]] esp_err_t Initialize(uint32_t slot_bytes, uint32_t slot_count);
    [[nodiscard]] bool Ready() const { return slot_count_ != 0U; }
    // Usable bytes in every slot (0 before Initialize).
    [[nodiscard]] uint32_t slot_bytes() const { return slot_bytes_; }

    // nullptr when the pool is not initialized, the caller needs more than
    // slot_bytes(), or fewer than keep_free slots would stay free afterwards.
    // Opportunistic users (screen capture) pass keep_free so the display
    // transitions the pool is sized for never find it drained; they get no
    // exhaustion warning, the pool simply declines.
    [[nodiscard]] uint8_t* Acquire(uint32_t bytes, uint32_t keep_free = 0U);
    // No-op for nullptr or pointers that do not belong to the pool.
    void Release(uint8_t* stage);
    [[nodiscard]] bool Owns(const uint8_t* stage) const;

   private:
    ScanoutStagePool() = default;
    // Slots currently acquired; caller holds lock_.
    [[nodiscard]] uint32_t InUseLocked() const;

    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    uint8_t* slots_[kMaxSlots]{};
    uint32_t slot_bytes_{};
    uint32_t slot_count_{};
    uint32_t in_use_mask_{};
};

}  // namespace micropixel::platform::lvgl
