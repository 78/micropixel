#pragma once

#include <cstdint>

#include "esp_err.h"

namespace micropixel::platform::lvgl {

struct DisplayGeometry final {
    uint32_t width{};
    uint32_t height{};
    uint32_t bytes_per_pixel{};
};

struct DisplayCapabilities final {
    bool partial_flush{};
    bool tearing_effect_sync{};
    bool direct_framebuffers{};
    bool ppa{};
    bool dma2d{};
    bool hardware_jpeg{};
};

struct SystemTransitionProfile final {
    uint32_t intermediate_width{};
    uint32_t card_width{};
    float fullscreen_to_intermediate_scale{};
    float intermediate_to_card_scale{};
};

// How a Direct Surface (Graphics 1.5) reaches the panel on this pipeline.
//   kComposited      copy into the App Surface and let LVGL present it;
//   kBlitRgb565      dummy-draw blit of the RGB565 frame straight to the
//                    controller (QSPI/SPI panels);
//   kFramebufferRgb888 PPA-convert into a free DPI framebuffer and flip it.
struct DirectScanoutProfile final {
    enum class Mode : uint8_t { kComposited, kBlitRgb565, kFramebufferRgb888 };
    Mode mode{Mode::kComposited};
    // The controller consumes RGB565 with the bytes of every pixel swapped.
    bool rgb565_byte_swapped{};
    // Panel transfer bound for one full frame; reported to the Guest.
    uint16_t max_full_frame_fps{};
    // Partial-window blits bypass the LVGL area rounder, so the controller's
    // column/row window granularity is repeated here (1 = no constraint).
    uint8_t blit_x_alignment{1U};
    uint8_t blit_y_alignment{1U};
};

// Optional fast-path owned by a concrete display pipeline. QSPI panels that
// only expose controller GRAM return no implementation; shared renderers must
// then stay on the ordinary LVGL partial-flush path.
class DirectFramebufferAccess {
   public:
    virtual ~DirectFramebufferAccess() = default;
    DirectFramebufferAccess(const DirectFramebufferAccess&) = delete;
    DirectFramebufferAccess& operator=(const DirectFramebufferAccess&) = delete;

    [[nodiscard]] virtual bool Ready() const = 0;
    [[nodiscard]] virtual uint32_t Count() const = 0;
    [[nodiscard]] virtual uint8_t* AcquireFree() = 0;
    [[nodiscard]] virtual uint8_t* Displayed() = 0;
    [[nodiscard]] virtual bool Contains(const uint8_t* buffer) const = 0;
    [[nodiscard]] virtual esp_err_t Submit(uint8_t* buffer) = 0;

   protected:
    DirectFramebufferAccess() = default;
};

// Platform-internal display boundary. Device/Runtime still see only
// device::Graphics; this contract keeps panel transport, brightness and
// framebuffer assumptions out of the reusable LVGL renderer.
class DisplayPipeline {
   public:
    virtual ~DisplayPipeline() = default;
    DisplayPipeline(const DisplayPipeline&) = delete;
    DisplayPipeline& operator=(const DisplayPipeline&) = delete;

    [[nodiscard]] virtual DisplayGeometry Geometry() const = 0;
    [[nodiscard]] virtual DisplayCapabilities Capabilities() const = 0;
    [[nodiscard]] virtual DirectFramebufferAccess* DirectFramebuffers() = 0;
    [[nodiscard]] virtual DirectScanoutProfile DirectScanout() const { return {}; }
    [[nodiscard]] virtual esp_err_t Suspend() = 0;
    [[nodiscard]] virtual esp_err_t Resume() = 0;
    [[nodiscard]] virtual esp_err_t SetBrightness(uint32_t per_ten_thousand) = 0;

   protected:
    DisplayPipeline() = default;
};

}  // namespace micropixel::platform::lvgl
