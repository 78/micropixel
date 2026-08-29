#include "platform/boards/esp-mosaico/display/display_pipeline.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <iterator>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "platform/boards/esp-mosaico/board_config.hpp"
#include "platform/boards/esp-mosaico/display/brightness_controller.hpp"
#include "src/display/lv_display_private.h"

namespace micropixel::platform::esp_mosaico {
namespace {

constexpr char kTag[] = "mosaico_display";
constexpr uint32_t kAllocationAlignment = 128U;

}  // namespace

esp_err_t MosaicoDisplayPipeline::InitializePanel() {
    if (geometry_.width != static_cast<uint32_t>(board::kDisplayWidth) ||
        geometry_.height != static_cast<uint32_t>(board::kDisplayHeight)) {
        return ESP_ERR_INVALID_ARG;
    }

    static const uint8_t kPage20[] = {0x20};
    static const uint8_t kRegister19[] = {0x10};
    static const uint8_t kRegister1c[] = {0xa0};
    static const uint8_t kPage00[] = {0x00};
    static const uint8_t kRegisterC4[] = {0x80};
    static const uint8_t kPixelFormat[] = {0x55};
    static const uint8_t kTearEffect[] = {0x00};
    static const uint8_t kDisplayControl[] = {0x20};
    static const uint8_t kMaximumBrightness[] = {0xff};
    static const uint8_t kHbmBrightness[] = {0xff};
    static const uint8_t kColumnRange[] = {0x00, 0x00, 0x01, 0xdf};
    static const uint8_t kRowRange[] = {0x00, 0x00, 0x01, 0xdf};
    static const co5300_lcd_init_cmd_t kVendorInit[] = {
        {0x11, nullptr, 0, 600},
        {0xfe, kPage20, sizeof(kPage20), 0},
        {0x19, kRegister19, 1, 0},
        {0x1c, kRegister1c, 1, 0},
        {0xfe, kPage00, sizeof(kPage00), 0},
        {0xc4, kRegisterC4, 1, 0},
        {0x3a, kPixelFormat, 1, 0},
        {0x35, kTearEffect, 1, 0},
        {0x53, kDisplayControl, 1, 0},
        {0x51, kMaximumBrightness, 1, 0},
        {0x63, kHbmBrightness, 1, 0},
        {0x2a, kColumnRange, 4, 0},
        {0x2b, kRowRange, 4, 0},
        {0x29, nullptr, 0, 600},
    };

    spi_bus_config_t bus_config{};
    bus_config.data0_io_num = board::kDisplayData0;
    bus_config.data1_io_num = board::kDisplayData1;
    bus_config.sclk_io_num = board::kDisplayClock;
    bus_config.data2_io_num = board::kDisplayData2;
    bus_config.data3_io_num = board::kDisplayData3;
    bus_config.data4_io_num = -1;
    bus_config.data5_io_num = -1;
    bus_config.data6_io_num = -1;
    bus_config.data7_io_num = -1;
    bus_config.max_transfer_sz = board::kDisplayWidth * CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT *
                                 static_cast<int32_t>(board::kDisplayBitsPerPixel) / 8;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(board::kDisplaySpiHost, &bus_config, SPI_DMA_CH_AUTO), kTag,
                        "initialize CO5300 QSPI bus failed");
    constexpr gpio_num_t kBusPins[]{board::kDisplayClock, board::kDisplayData0, board::kDisplayData1,
                                    board::kDisplayData2, board::kDisplayData3};
    for (gpio_num_t pin : kBusPins) {
        ESP_RETURN_ON_ERROR(
            gpio_set_drive_capability(pin, static_cast<gpio_drive_cap_t>(CONFIG_MICROPIXEL_MOSAICO_LCD_QSPI_DRIVE_CAP)),
            kTag, "set QSPI GPIO%d drive capability failed", static_cast<int>(pin));
    }

    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = board::kDisplayChipSelect;
    io_config.dc_gpio_num = GPIO_NUM_NC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = board::kDisplayPixelClockHz;
    io_config.trans_queue_depth = 10U;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = true;
    io_config.flags.psram_dma_direct = true;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(board::kDisplaySpiHost), &io_config, &panel_io_),
        kTag, "create CO5300 panel IO failed");

    co5300_vendor_config_t vendor_config{};
    vendor_config.init_cmds = kVendorInit;
    vendor_config.init_cmds_size = std::size(kVendorInit);
    vendor_config.flags.use_qspi_interface = true;
    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = board::kDisplayReset;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = board::kDisplayBitsPerPixel;
    panel_config.vendor_config = &vendor_config;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(panel_io_, &panel_config, &panel_), kTag,
                        "create CO5300 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_), kTag, "reset CO5300 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_), kTag, "initialize CO5300 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel_, true), kTag, "turn CO5300 on failed");
    ESP_LOGI(kTag, "CO5300 pipeline ready: %" PRIu32 "x%" PRIu32 " QSPI with TE on GPIO%d", geometry_.width,
             geometry_.height, static_cast<int>(board::kDisplayTe));
    return ESP_OK;
}

esp_err_t MosaicoDisplayPipeline::BindLvgl(lv_display_t* display) {
    if (display == nullptr || panel_ == nullptr || panel_io_ == nullptr || display_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t row_bytes = geometry_.width * geometry_.bytes_per_pixel;
    const uint64_t frame_bytes = static_cast<uint64_t>(row_bytes) * geometry_.height;
    if (frame_bytes == 0U || frame_bytes > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t allocation_bytes =
        (static_cast<uint32_t>(frame_bytes) + kAllocationAlignment - 1U) / kAllocationAlignment * kAllocationAlignment;
    displayed_shadow_ = static_cast<uint8_t*>(
        heap_caps_aligned_calloc(kAllocationAlignment, allocation_bytes, 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (displayed_shadow_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    async_color_convert_config_t dma2d_config{};
    dma2d_config.backlog = 1U;
    dma2d_config.dma_burst_size = 128U;
    ESP_RETURN_ON_ERROR(esp_async_color_convert_install_dma2d(&dma2d_config, &shadow_copy_dma2d_), kTag,
                        "install displayed-shadow DMA2D copier failed");

    display_ = display;
    adapter_flush_cb_ = display_->flush_cb;
    if (adapter_flush_cb_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    flush_owner_ = this;
    lv_display_set_flush_cb(display_, Flush);
    ESP_RETURN_ON_ERROR(esp_lv_adapter_set_area_rounder_cb(display_, RoundArea, this), kTag,
                        "configure QSPI area alignment failed");
    return ESP_OK;
}

lvgl::DisplayCapabilities MosaicoDisplayPipeline::Capabilities() const {
    return {.partial_flush = true,
            .tearing_effect_sync = false,
            .direct_framebuffers = false,
            .ppa = true,
            .dma2d = true,
            .hardware_jpeg = true};
}

esp_err_t MosaicoDisplayPipeline::Suspend() {
    return panel_ == nullptr ? ESP_ERR_INVALID_STATE : esp_lcd_panel_disp_on_off(panel_, false);
}

esp_err_t MosaicoDisplayPipeline::Resume() {
    return panel_ == nullptr ? ESP_ERR_INVALID_STATE : esp_lcd_panel_disp_on_off(panel_, true);
}

esp_err_t MosaicoDisplayPipeline::SetBrightness(uint32_t per_ten_thousand) {
    const uint32_t bounded = std::min<uint32_t>(per_ten_thousand, 10000U);
    return SetDisplayBrightness(panel_, static_cast<uint8_t>((bounded + 50U) / 100U));
}

void MosaicoDisplayPipeline::Flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    MosaicoDisplayPipeline* owner = flush_owner_;
    bool shadow_copied = false;
    if (owner != nullptr && owner->display_ == display) {
        shadow_copied = owner->CaptureDisplayedShadow(display, area, pixels);
        if (area != nullptr && area->x2 >= area->x1 && area->y2 >= area->y1) {
            const uint64_t area_pixels =
                static_cast<uint64_t>(area->x2 - area->x1 + 1) * static_cast<uint64_t>(area->y2 - area->y1 + 1);
            ++owner->panel_submit_count_;
            owner->panel_submit_pixels_ += area_pixels;
            if (shadow_copied) {
                ++owner->shadow_copy_count_;
                owner->shadow_copy_pixels_ += area_pixels;
            }
            if (owner->panel_submit_count_ <= 8U || (owner->panel_submit_count_ % 120U) == 0U) {
                ESP_LOGI(kTag,
                         "panel submit #%" PRIu32 ": area=%dx%d+%d+%d shadow=%s totals=submits:%" PRIu32 "/%" PRIu64
                         "px copies:%" PRIu32 "/%" PRIu64 "px",
                         owner->panel_submit_count_, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1, area->x1,
                         area->y1, shadow_copied ? "dma2d" : "none", owner->panel_submit_count_,
                         owner->panel_submit_pixels_, owner->shadow_copy_count_, owner->shadow_copy_pixels_);
            }
        }
    }
    if (owner != nullptr && owner->adapter_flush_cb_ != nullptr) {
        owner->adapter_flush_cb_(display, area, pixels);
    } else {
        lv_display_flush_ready(display);
    }
}

void MosaicoDisplayPipeline::RoundArea(lv_area_t* area, void*) {
    if (area == nullptr) {
        return;
    }
    area->x1 = std::max<int32_t>(0, area->x1 / 4 * 4);
    area->x2 = std::min<int32_t>(static_cast<int32_t>(board::kDisplayWidth) - 1, (area->x2 + 4) / 4 * 4 - 1);
    area->y1 = std::max<int32_t>(0, area->y1 / 2 * 2);
    area->y2 = std::min<int32_t>(static_cast<int32_t>(board::kDisplayHeight) - 1, (area->y2 + 2) / 2 * 2 - 1);
}

bool MosaicoDisplayPipeline::CaptureDisplayedShadow(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    if (displayed_shadow_ == nullptr || area == nullptr || pixels == nullptr || area->x1 < 0 || area->y1 < 0 ||
        area->x2 >= static_cast<int32_t>(geometry_.width) || area->y2 >= static_cast<int32_t>(geometry_.height)) {
        return false;
    }
    const uint32_t area_width = static_cast<uint32_t>(area->x2 - area->x1 + 1);
    const uint32_t area_height = static_cast<uint32_t>(area->y2 - area->y1 + 1);
    const lv_draw_buf_t* active = lv_display_get_buf_active(display);
    if (active == nullptr || active->header.stride % 2U != 0U) {
        return false;
    }
    async_color_convert_request_t copy{};
    copy.src_buffer = pixels;
    copy.src_stride = active->header.stride / 2U;
    copy.src_height = area_height;
    copy.dst_buffer = displayed_shadow_;
    copy.dst_stride = geometry_.width;
    copy.dst_height = geometry_.height;
    copy.dst_x = static_cast<uint32_t>(area->x1);
    copy.dst_y = static_cast<uint32_t>(area->y1);
    copy.copy_width = area_width;
    copy.copy_height = area_height;
    copy.src_color_format = ESP_COLOR_FOURCC_RGB16;
    copy.dst_color_format = ESP_COLOR_FOURCC_RGB16;
    const bool copied =
        shadow_copy_dma2d_ != nullptr && esp_color_convert_blocking(shadow_copy_dma2d_, &copy, -1) == ESP_OK;
    if (!copied || area->x1 != 0 || area->x2 != static_cast<int32_t>(geometry_.width) - 1) {
        return copied;
    }
    for (int32_t y = area->y1; y <= area->y2; ++y) {
        displayed_shadow_rows_[static_cast<size_t>(y)] = true;
    }
    if (!displayed_shadow_valid_) {
        displayed_shadow_valid_ =
            std::all_of(displayed_shadow_rows_.begin(), displayed_shadow_rows_.begin() + geometry_.height,
                        [](bool ready) { return ready; });
    }
    return true;
}

}  // namespace micropixel::platform::esp_mosaico
