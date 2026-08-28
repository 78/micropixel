#pragma once

#include <cstdint>

#include "driver/ppa.h"
#include "esp_err.h"

namespace micropixel::platform::lvgl {

struct PpaSrmRect final {
    uint32_t x{};
    uint32_t y{};
    uint32_t width{};
    uint32_t height{};
};

struct PpaSrmBlit final {
    const void* source{};
    uint32_t source_width{};
    uint32_t source_height{};
    PpaSrmRect source_region{};
    ppa_srm_color_mode_t source_mode{};
    void* destination{};
    uint32_t destination_width{};
    uint32_t destination_height{};
    uint32_t destination_allocation_bytes{};
    uint32_t destination_x{};
    uint32_t destination_y{};
    ppa_srm_color_mode_t destination_mode{};
    float scale_x{1.0F};
    float scale_y{1.0F};
    // PPA applies this before decoding the source pixel. It is not an output
    // byte-order option and must not be used to pack a scaled result.
    bool input_byte_swap{};
};

// Shared blocking PPA scale/rotate/mirror frontend for the P4 and S31 display
// pipelines. Callers own image lifetime and board-specific presentation; this
// class centralizes buffer bounds, color modes and byte-order configuration.
class PpaSrmBlitter final {
   public:
    PpaSrmBlitter() = default;
    PpaSrmBlitter(const PpaSrmBlitter&) = delete;
    PpaSrmBlitter& operator=(const PpaSrmBlitter&) = delete;
    ~PpaSrmBlitter();

    [[nodiscard]] esp_err_t Initialize(uint32_t max_pending_transactions = 1U);
    [[nodiscard]] esp_err_t Blit(const PpaSrmBlit& request) const;
    // Materialize a byte-swapped RGB565 representation using a separate 1:1
    // hardware pass. Source and destination must not overlap.
    [[nodiscard]] esp_err_t SwapRgb565Bytes(const void* source, void* destination, uint32_t width, uint32_t height,
                                            uint32_t destination_allocation_bytes) const;
    [[nodiscard]] bool Ready() const { return client_ != nullptr; }
    void Release();

   private:
    ppa_client_handle_t client_{};
};

}  // namespace micropixel::platform::lvgl
