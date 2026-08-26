#include "platform/metalio-claw4/display/system_transition_compositor.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "src/core/lv_obj_draw_private.h"
#include "src/draw/snapshot/lv_snapshot.h"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "system_transition";
constexpr uint32_t kPpaBufferAlignment = 128U;

constexpr uint32_t AlignPpaBufferSize(uint32_t size) {
    return (size + kPpaBufferAlignment - 1U) / kPpaBufferAlignment * kPpaBufferAlignment;
}

int32_t Interpolate(int32_t from, int32_t to, uint32_t numerator, uint32_t denominator) {
    return from + static_cast<int32_t>((static_cast<int64_t>(to - from) * numerator) / denominator);
}

}  // namespace

SystemTransitionCompositor::~SystemTransitionCompositor() { Release(); }

esp_err_t SystemTransitionCompositor::Initialize(lv_display_t* display, esp_lcd_panel_handle_t panel, uint32_t width,
                                                 uint32_t height) {
    if (display == nullptr || panel == nullptr || width == 0U || height == 0U || width > UINT32_MAX / kBytesPerPixel ||
        width * kBytesPerPixel > UINT32_MAX / height) {
        return ESP_ERR_INVALID_ARG;
    }
    if (srm_client_ != nullptr) {
        return display_ == display && panel_ == panel && width_ == width && height_ == height ? ESP_OK
                                                                                              : ESP_ERR_INVALID_STATE;
    }

    ppa_client_config_t config{
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1U,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
        .flags = {},
    };
    esp_err_t status = ppa_register_client(&config, &srm_client_);
    if (status != ESP_OK) {
        return status;
    }

    config.oper_type = PPA_OPERATION_BLEND;
    status = ppa_register_client(&config, &blend_client_);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "PPA status-layer blender unavailable: %s", esp_err_to_name(status));
        blend_client_ = nullptr;
    }

    async_color_convert_config_t dma2d_config{};
    dma2d_config.backlog = 1U;
    dma2d_config.dma_burst_size = 128U;
    status = esp_async_color_convert_install_dma2d(&dma2d_config, &dma2d_client_);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "DMA2D background copier unavailable; using PPA SRM fallback: %s", esp_err_to_name(status));
        dma2d_client_ = nullptr;
    }
    display_ = display;
    panel_ = panel;
    width_ = width;
    height_ = height;
    frame_bytes_ = width * height * kBytesPerPixel;
    ESP_LOGI(kTag,
             "transition compositor ready: frame=%" PRIu32 "x%" PRIu32 " bytes=%" PRIu32
             " scale=ppa background-copy=%s status-layer=%s",
             width_, height_, frame_bytes_, dma2d_client_ != nullptr ? "dma2d" : "ppa",
             blend_client_ != nullptr ? "ppa-blend+dma2d" : "lvgl");
    return ESP_OK;
}

void SystemTransitionCompositor::RebindPanel(esp_lcd_panel_handle_t panel) {
    panel_ = panel;
    status_frame_states_ = {};
    prepared_to_hall_ = false;
}

bool SystemTransitionCompositor::PrepareBackgroundLocked(lv_obj_t* root) { return CaptureBackgroundLocked(root, true); }

bool SystemTransitionCompositor::RefreshBackgroundLocked(lv_obj_t* root) {
    return CaptureBackgroundLocked(root, false);
}

bool SystemTransitionCompositor::CaptureBackgroundLocked(lv_obj_t* root, bool preserve_as_baseline) {
    if (display_ == nullptr || srm_client_ == nullptr || root == nullptr) {
        return false;
    }
    if (background_pixels_ == nullptr) {
        background_pixels_ = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(kPpaBufferAlignment, frame_bytes_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (background_pixels_ == nullptr) {
            ESP_LOGE(kTag, "could not allocate Hall transition background: bytes=%" PRIu32, frame_bytes_);
            return false;
        }
    }
    if (preserve_as_baseline && baseline_background_pixels_ == nullptr) {
        baseline_background_pixels_ = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(kPpaBufferAlignment, frame_bytes_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (baseline_background_pixels_ == nullptr) {
            ESP_LOGE(kTag, "could not allocate baseline Hall background: bytes=%" PRIu32, frame_bytes_);
            ClearBackground();
            return false;
        }
    }

    lv_draw_buf_t snapshot{};
    const uint32_t stride = width_ * kBytesPerPixel;
    const bool captured = lv_draw_buf_init(&snapshot, width_, height_, LV_COLOR_FORMAT_RGB888, stride,
                                           background_pixels_, frame_bytes_) == LV_RESULT_OK &&
                          lv_snapshot_take_to_draw_buf(root, LV_COLOR_FORMAT_RGB888, &snapshot) == LV_RESULT_OK;
    if (!captured || snapshot.header.w != width_ || snapshot.header.h != height_ || snapshot.header.stride != stride) {
        ESP_LOGE(kTag, "could not render Hall transition background");
        if (preserve_as_baseline || !ResetBackgroundToBaseline()) {
            ClearBackground();
        }
        return false;
    }
    lv_draw_buf_flush_cache(&snapshot, nullptr);
    if (preserve_as_baseline && !CopyRgb888(background_pixels_, width_, height_, 0U, 0U, baseline_background_pixels_,
                                            width_, height_, 0U, 0U, width_, height_)) {
        ESP_LOGE(kTag, "could not preserve baseline Hall background");
        ClearBackground();
        return false;
    }
    return true;
}

bool SystemTransitionCompositor::ResetBackgroundToBaseline() {
    return background_pixels_ != nullptr && baseline_background_pixels_ != nullptr &&
           CopyRgb888(baseline_background_pixels_, width_, height_, 0U, 0U, background_pixels_, width_, height_, 0U, 0U,
                      width_, height_);
}

bool SystemTransitionCompositor::UpdateBackgroundRegionLocked(lv_obj_t* root, const SystemTransitionRect& region) {
    if (root != nullptr) {
        lv_obj_update_layout(root);
    }
    if (background_pixels_ == nullptr || root == nullptr || region.x < 0 || region.y < 0 || region.width <= 0 ||
        region.height <= 0 || region.x + region.width > static_cast<int32_t>(width_) ||
        region.y + region.height > static_cast<int32_t>(height_) || lv_obj_get_width(root) != region.width ||
        lv_obj_get_height(root) != region.height) {
        ESP_LOGW(kTag,
                 "Hall background region rejected: root=%p region=%" PRId32 ",%" PRId32 " %" PRId32 "x%" PRId32
                 " object=%" PRId32 "x%" PRId32,
                 root, region.x, region.y, region.width, region.height, root != nullptr ? lv_obj_get_width(root) : 0,
                 root != nullptr ? lv_obj_get_height(root) : 0);
        return false;
    }

    const int32_t ext_draw_size = lv_obj_get_ext_draw_size(root);
    const int32_t snapshot_x = region.x - ext_draw_size;
    const int32_t snapshot_y = region.y - ext_draw_size;
    const int32_t snapshot_width = region.width + ext_draw_size * 2;
    const int32_t snapshot_height = region.height + ext_draw_size * 2;
    if (ext_draw_size < 0 || snapshot_x < 0 || snapshot_y < 0 || snapshot_width <= 0 || snapshot_height <= 0 ||
        snapshot_x + snapshot_width > static_cast<int32_t>(width_) ||
        snapshot_y + snapshot_height > static_cast<int32_t>(height_)) {
        ESP_LOGW(kTag, "Hall background snapshot bounds rejected: ext=%" PRId32, ext_draw_size);
        return false;
    }

    const uint32_t scratch_bytes =
        static_cast<uint32_t>(snapshot_width) * static_cast<uint32_t>(snapshot_height) * kBytesPerPixel;
    const uint32_t scratch_allocation_bytes = AlignPpaBufferSize(scratch_bytes);
    auto* scratch = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(kPpaBufferAlignment, scratch_allocation_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr) {
        return false;
    }

    lv_draw_buf_t snapshot{};
    const uint32_t scratch_stride = static_cast<uint32_t>(snapshot_width) * kBytesPerPixel;
    const bool captured =
        lv_draw_buf_init(&snapshot, static_cast<uint32_t>(snapshot_width), static_cast<uint32_t>(snapshot_height),
                         LV_COLOR_FORMAT_RGB888, scratch_stride, scratch, scratch_bytes) == LV_RESULT_OK &&
        lv_snapshot_take_to_draw_buf(root, LV_COLOR_FORMAT_RGB888, &snapshot) == LV_RESULT_OK;
    if (captured) {
        lv_draw_buf_flush_cache(&snapshot, nullptr);
    }
    const bool copied =
        captured && CopyRgb888(scratch, static_cast<uint32_t>(snapshot_width), static_cast<uint32_t>(snapshot_height),
                               0U, 0U, background_pixels_, width_, height_, static_cast<uint32_t>(snapshot_x),
                               static_cast<uint32_t>(snapshot_y), static_cast<uint32_t>(snapshot_width),
                               static_cast<uint32_t>(snapshot_height));
    if (!copied) {
        ESP_LOGW(kTag, "Hall background region update failed: captured=%s ext=%" PRId32, captured ? "yes" : "no",
                 ext_draw_size);
    }
    heap_caps_free(scratch);
    return copied;
}

bool SystemTransitionCompositor::UpdateBackgroundPixels(const uint8_t* pixels, const SystemTransitionRect& region) {
    if (background_pixels_ == nullptr || pixels == nullptr || region.x < 0 || region.y < 0 || region.width <= 0 ||
        region.height <= 0 || region.x + region.width > static_cast<int32_t>(width_) ||
        region.y + region.height > static_cast<int32_t>(height_)) {
        return false;
    }
    return CopyRgb888(pixels, static_cast<uint32_t>(region.width), static_cast<uint32_t>(region.height), 0U, 0U,
                      background_pixels_, width_, height_, static_cast<uint32_t>(region.x),
                      static_cast<uint32_t>(region.y), static_cast<uint32_t>(region.width),
                      static_cast<uint32_t>(region.height));
}

uint8_t* SystemTransitionCompositor::DisplayedFrameBuffer() const {
    if (display_ == nullptr || panel_ == nullptr) {
        return nullptr;
    }
    constexpr uint32_t kFramebufferCount = 2U;
    void* panel_frame_buffers[kFramebufferCount]{};
    const esp_err_t status =
        esp_lcd_dpi_panel_get_frame_buffer(panel_, kFramebufferCount, &panel_frame_buffers[0], &panel_frame_buffers[1]);
    auto* free_frame_buffer = static_cast<uint8_t*>(esp_lv_adapter_dummy_draw_get_free_buf_preserve(display_));
    if (status == ESP_OK && free_frame_buffer == panel_frame_buffers[0]) {
        return static_cast<uint8_t*>(panel_frame_buffers[1]);
    }
    if (status == ESP_OK && free_frame_buffer == panel_frame_buffers[1]) {
        return static_cast<uint8_t*>(panel_frame_buffers[0]);
    }
    return nullptr;
}

bool SystemTransitionCompositor::CaptureDisplayedToHalf(uint8_t* half, uint32_t half_allocation_bytes,
                                                        const SystemTransitionRect& card, uint64_t trigger_timestamp_us,
                                                        uint32_t& elapsed_us) {
    elapsed_us = 0U;
    if (display_ == nullptr || panel_ == nullptr || half == nullptr || background_pixels_ == nullptr ||
        card.width != kCardWidth || card.height != kCardWidth || prepared_to_hall_) {
        return false;
    }

    const int64_t started_us = esp_timer_get_time();
    esp_err_t status = esp_lv_adapter_set_dummy_draw(display_, true);
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "could not freeze displayed Guest for capture: %s", esp_err_to_name(status));
        return false;
    }
    uint8_t* displayed_frame_buffer = DisplayedFrameBuffer();

    const bool scaled =
        displayed_frame_buffer != nullptr && ScaleFullscreenToHalf(displayed_frame_buffer, half, half_allocation_bytes);

    constexpr uint32_t kInitialScaleUnits = 30U;
    auto* transition_frame =
        scaled ? static_cast<uint8_t*>(esp_lv_adapter_dummy_draw_get_free_buf_preserve(display_)) : nullptr;
    const SystemTransitionRect fullscreen_region{
        .x = 0, .y = 0, .width = static_cast<int32_t>(width_), .height = static_cast<int32_t>(height_)};
    const SystemTransitionRect initial_region = GuestRect(card, kInitialScaleUnits);
    SystemTransitionRect rendered_region{};
    const bool presented = transition_frame != nullptr &&
                           RestoreGuestDifference(transition_frame, fullscreen_region, initial_region) &&
                           ComposeGuest(transition_frame, half, card, kInitialScaleUnits, rendered_region) &&
                           SubmitFrame(transition_frame);
    elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
    if (!presented) {
        const esp_err_t restore_status = esp_lv_adapter_set_dummy_draw(display_, false);
        ESP_LOGE(kTag, "displayed Guest capture failed: buffers=%s refresh=%s",
                 displayed_frame_buffer != nullptr ? "resolved" : "unresolved", esp_err_to_name(restore_status));
        return false;
    }
    prepared_to_hall_ = true;
    if (trigger_timestamp_us != 0U && static_cast<uint64_t>(esp_timer_get_time()) >= trigger_timestamp_us) {
        ESP_LOGI(kTag,
                 "transition responsiveness: direction=to-hall phase=initial scale=%" PRIu32
                 "/16 trigger-to-first-present=%" PRIu64 " us capture+present=%" PRIu32 " us",
                 kInitialScaleUnits, static_cast<uint64_t>(esp_timer_get_time()) - trigger_timestamp_us, elapsed_us);
    }
    return true;
}

bool SystemTransitionCompositor::ScaleRgb888(const uint8_t* source, uint32_t source_width, uint32_t source_height,
                                             uint8_t* destination, uint32_t destination_width,
                                             uint32_t destination_height, uint32_t destination_allocation_bytes,
                                             uint32_t destination_x, uint32_t destination_y, float scale) {
    return BlitRgb888(source, source_width, source_height, 0U, 0U, source_width, source_height, destination,
                      destination_width, destination_height, destination_allocation_bytes, destination_x, destination_y,
                      scale);
}

bool SystemTransitionCompositor::CopyRgb888(const uint8_t* source, uint32_t source_width, uint32_t source_height,
                                            uint32_t source_x, uint32_t source_y, uint8_t* destination,
                                            uint32_t destination_width, uint32_t destination_height,
                                            uint32_t destination_x, uint32_t destination_y, uint32_t copy_width,
                                            uint32_t copy_height) {
    if (source == nullptr || destination == nullptr || copy_width == 0U || copy_height == 0U ||
        source_x >= source_width || source_y >= source_height || copy_width > source_width - source_x ||
        copy_height > source_height - source_y || destination_x >= destination_width ||
        destination_y >= destination_height || copy_width > destination_width - destination_x ||
        copy_height > destination_height - destination_y) {
        return false;
    }
    if (dma2d_client_ != nullptr) {
        async_color_convert_request_t copy{};
        copy.src_buffer = source;
        copy.src_stride = source_width;
        copy.src_height = source_height;
        copy.src_x = source_x;
        copy.src_y = source_y;
        copy.dst_buffer = destination;
        copy.dst_stride = destination_width;
        copy.dst_height = destination_height;
        copy.dst_x = destination_x;
        copy.dst_y = destination_y;
        copy.copy_width = copy_width;
        copy.copy_height = copy_height;
        copy.src_color_format = ESP_COLOR_FOURCC_BGR24;
        copy.dst_color_format = ESP_COLOR_FOURCC_BGR24;
        if (esp_color_convert_blocking(dma2d_client_, &copy, -1) == ESP_OK) {
            return true;
        }
    }
    return BlitRgb888(source, source_width, source_height, source_x, source_y, copy_width, copy_height, destination,
                      destination_width, destination_height, destination_width * destination_height * kBytesPerPixel,
                      destination_x, destination_y, 1.0F);
}

bool SystemTransitionCompositor::BlitRgb888(const uint8_t* source, uint32_t source_width, uint32_t source_height,
                                            uint32_t source_x, uint32_t source_y, uint32_t source_block_width,
                                            uint32_t source_block_height, uint8_t* destination,
                                            uint32_t destination_width, uint32_t destination_height,
                                            uint32_t destination_allocation_bytes, uint32_t destination_x,
                                            uint32_t destination_y, float scale) {
    if (srm_client_ == nullptr || source == nullptr || destination == nullptr || source_width == 0U ||
        source_height == 0U || source_block_width == 0U || source_block_height == 0U || source_x >= source_width ||
        source_y >= source_height || source_block_width > source_width - source_x ||
        source_block_height > source_height - source_y || destination_width == 0U || destination_height == 0U) {
        return false;
    }
    ppa_srm_oper_config_t config{};
    config.in.buffer = source;
    config.in.pic_w = source_width;
    config.in.pic_h = source_height;
    config.in.block_offset_x = source_x;
    config.in.block_offset_y = source_y;
    config.in.block_w = source_block_width;
    config.in.block_h = source_block_height;
    config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
    config.out.buffer = destination;
    config.out.buffer_size = destination_allocation_bytes;
    config.out.pic_w = destination_width;
    config.out.pic_h = destination_height;
    config.out.block_offset_x = destination_x;
    config.out.block_offset_y = destination_y;
    config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
    config.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    config.scale_x = scale;
    config.scale_y = scale;
    config.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t status = ppa_do_scale_rotate_mirror(srm_client_, &config);
    if (status != ESP_OK) {
        ESP_LOGE(kTag,
                 "PPA RGB888 blit failed: %s source=%" PRIu32 "x%" PRIu32 " block=%" PRIu32 ",%" PRIu32 " %" PRIu32
                 "x%" PRIu32 " scale=%.4f destination=%" PRIu32 ",%" PRIu32,
                 esp_err_to_name(status), source_width, source_height, source_x, source_y, source_block_width,
                 source_block_height, static_cast<double>(scale), destination_x, destination_y);
        return false;
    }
    return true;
}

bool SystemTransitionCompositor::ScaleFullscreenToHalf(const uint8_t* fullscreen, uint8_t* half,
                                                       uint32_t half_allocation_bytes) {
    if (width_ != 720U || height_ != 720U || fullscreen == nullptr || half == nullptr) {
        return false;
    }
    constexpr uint32_t kHalfBytes = kHalfWidth * kHalfWidth * kBytesPerPixel;
    constexpr uint32_t kHalfAllocationBytes = AlignPpaBufferSize(kHalfBytes);
    if (half_allocation_bytes < kHalfAllocationBytes) {
        return false;
    }

    return ScaleRgb888(fullscreen, width_, height_, half, kHalfWidth, kHalfWidth, half_allocation_bytes, 0U, 0U,
                       8.0F / 16.0F);
}

bool SystemTransitionCompositor::ScaleHalfToCard(const uint8_t* half, uint8_t* card, uint32_t card_allocation_bytes) {
    if (half == nullptr || card == nullptr) {
        return false;
    }
    // ESP32-P4 represents the fractional scale in sixteenths. Combined with
    // the exact 720 -> 360 pass above, floor(360 * 9/16) gives the 202px card.
    return ScaleRgb888(half, kHalfWidth, kHalfWidth, card, kCardWidth, kCardWidth, card_allocation_bytes, 0U, 0U,
                       9.0F / 16.0F);
}

bool SystemTransitionCompositor::ComposeBackground(uint8_t* frame_buffer) {
    return ComposeBackgroundRegion(
        frame_buffer, {.x = 0, .y = 0, .width = static_cast<int32_t>(width_), .height = static_cast<int32_t>(height_)});
}

bool SystemTransitionCompositor::ComposeBackgroundRegion(uint8_t* frame_buffer, const SystemTransitionRect& region) {
    if (region.x < 0 || region.y < 0 || region.width <= 0 || region.height <= 0 ||
        region.x + region.width > static_cast<int32_t>(width_) ||
        region.y + region.height > static_cast<int32_t>(height_)) {
        return false;
    }
    if (dma2d_client_ != nullptr) {
        async_color_convert_request_t copy{};
        copy.src_buffer = background_pixels_;
        copy.src_stride = width_;
        copy.src_height = height_;
        copy.src_x = static_cast<uint32_t>(region.x);
        copy.src_y = static_cast<uint32_t>(region.y);
        copy.dst_buffer = frame_buffer;
        copy.dst_stride = width_;
        copy.dst_height = height_;
        copy.dst_x = static_cast<uint32_t>(region.x);
        copy.dst_y = static_cast<uint32_t>(region.y);
        copy.copy_width = static_cast<uint32_t>(region.width);
        copy.copy_height = static_cast<uint32_t>(region.height);
        copy.src_color_format = ESP_COLOR_FOURCC_BGR24;
        copy.dst_color_format = ESP_COLOR_FOURCC_BGR24;
        const esp_err_t status = esp_color_convert_blocking(dma2d_client_, &copy, -1);
        if (status == ESP_OK) {
            return true;
        }
        ESP_LOGW(kTag, "DMA2D Hall background copy failed; using PPA SRM fallback: %s", esp_err_to_name(status));
    }
    return BlitRgb888(background_pixels_, width_, height_, static_cast<uint32_t>(region.x),
                      static_cast<uint32_t>(region.y), static_cast<uint32_t>(region.width),
                      static_cast<uint32_t>(region.height), frame_buffer, width_, height_, frame_bytes_,
                      static_cast<uint32_t>(region.x), static_cast<uint32_t>(region.y), 1.0F);
}

bool SystemTransitionCompositor::RestoreGuestDifference(uint8_t* frame_buffer, const SystemTransitionRect& old_region,
                                                        const SystemTransitionRect& new_region) {
    if (new_region.width >= old_region.width && new_region.height >= old_region.height) {
        return true;
    }

    const int32_t intersection_left = std::max(old_region.x, new_region.x);
    const int32_t intersection_top = std::max(old_region.y, new_region.y);
    const int32_t intersection_right = std::min(old_region.x + old_region.width, new_region.x + new_region.width);
    const int32_t intersection_bottom = std::min(old_region.y + old_region.height, new_region.y + new_region.height);
    if (intersection_left >= intersection_right || intersection_top >= intersection_bottom) {
        return ComposeBackgroundRegion(frame_buffer, old_region);
    }

    const std::array<SystemTransitionRect, 4U> exposed{{
        {.x = old_region.x, .y = old_region.y, .width = old_region.width, .height = intersection_top - old_region.y},
        {.x = old_region.x,
         .y = intersection_bottom,
         .width = old_region.width,
         .height = old_region.y + old_region.height - intersection_bottom},
        {.x = old_region.x,
         .y = intersection_top,
         .width = intersection_left - old_region.x,
         .height = intersection_bottom - intersection_top},
        {.x = intersection_right,
         .y = intersection_top,
         .width = old_region.x + old_region.width - intersection_right,
         .height = intersection_bottom - intersection_top},
    }};
    for (const auto& region : exposed) {
        if (region.width > 0 && region.height > 0 && !ComposeBackgroundRegion(frame_buffer, region)) {
            return ComposeBackgroundRegion(frame_buffer, old_region);
        }
    }
    return true;
}

SystemTransitionRect SystemTransitionCompositor::GuestRect(const SystemTransitionRect& card,
                                                           uint32_t scale_units) const {
    constexpr uint32_t kCardScaleUnits = 9U;
    constexpr uint32_t kFullscreenScaleUnits = 32U;
    const uint32_t scaled_width = kHalfWidth * scale_units / kPpaScaleDenominator;
    const uint32_t scaled_height = kHalfWidth * scale_units / kPpaScaleDenominator;
    const uint32_t progress_denominator = kFullscreenScaleUnits - kCardScaleUnits;
    const uint32_t progress_numerator = kFullscreenScaleUnits - scale_units;
    const int32_t screen_center_x = static_cast<int32_t>(width_ / 2U);
    const int32_t screen_center_y = static_cast<int32_t>(height_ / 2U);
    const int32_t card_center_x = card.x + card.width / 2;
    const int32_t card_center_y = card.y + card.height / 2;
    const int32_t center_x = Interpolate(screen_center_x, card_center_x, progress_numerator, progress_denominator);
    const int32_t center_y = Interpolate(screen_center_y, card_center_y, progress_numerator, progress_denominator);
    const int32_t destination_x = std::clamp<int32_t>(center_x - static_cast<int32_t>(scaled_width / 2U), int32_t{0},
                                                      static_cast<int32_t>(width_ - scaled_width));
    const int32_t destination_y = std::clamp<int32_t>(center_y - static_cast<int32_t>(scaled_height / 2U), int32_t{0},
                                                      static_cast<int32_t>(height_ - scaled_height));
    return {.x = destination_x,
            .y = destination_y,
            .width = static_cast<int32_t>(scaled_width),
            .height = static_cast<int32_t>(scaled_height)};
}

bool SystemTransitionCompositor::ComposeGuest(uint8_t* frame_buffer, const uint8_t* half_guest,
                                              const SystemTransitionRect& card, uint32_t scale_units,
                                              SystemTransitionRect& composed_region) {
    composed_region = GuestRect(card, scale_units);
    return ScaleRgb888(half_guest, kHalfWidth, kHalfWidth, frame_buffer, width_, height_, frame_bytes_,
                       static_cast<uint32_t>(composed_region.x), static_cast<uint32_t>(composed_region.y),
                       static_cast<float>(scale_units) / static_cast<float>(kPpaScaleDenominator));
}

bool SystemTransitionCompositor::SubmitFrame(uint8_t* frame_buffer) {
    const esp_err_t status = esp_lv_adapter_dummy_draw_flush_buf(display_, frame_buffer);
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "transition frame submit failed: %s", esp_err_to_name(status));
        return false;
    }
    return true;
}

bool SystemTransitionCompositor::ComposeStatusScrimRegion(uint8_t* frame_buffer, const SystemTransitionRect& region) {
    if (blend_client_ == nullptr || status_background_pixels_ == nullptr || status_scrim_alpha_pixels_ == nullptr ||
        frame_buffer == nullptr || region.x < 0 || region.y < 0 || region.width <= 0 || region.height <= 0 ||
        region.x + region.width > static_cast<int32_t>(width_) ||
        region.y + region.height > static_cast<int32_t>(height_)) {
        return false;
    }
    ppa_blend_oper_config_t config{};
    config.in_bg.buffer = status_background_pixels_;
    config.in_bg.pic_w = width_;
    config.in_bg.pic_h = height_;
    config.in_bg.block_w = static_cast<uint32_t>(region.width);
    config.in_bg.block_h = static_cast<uint32_t>(region.height);
    config.in_bg.block_offset_x = static_cast<uint32_t>(region.x);
    config.in_bg.block_offset_y = static_cast<uint32_t>(region.y);
    config.in_bg.blend_cm = PPA_BLEND_COLOR_MODE_RGB888;
    config.in_fg.buffer = status_scrim_alpha_pixels_;
    config.in_fg.pic_w = width_;
    config.in_fg.pic_h = height_;
    config.in_fg.block_w = static_cast<uint32_t>(region.width);
    config.in_fg.block_h = static_cast<uint32_t>(region.height);
    config.in_fg.block_offset_x = static_cast<uint32_t>(region.x);
    config.in_fg.block_offset_y = static_cast<uint32_t>(region.y);
    config.in_fg.blend_cm = PPA_BLEND_COLOR_MODE_A8;
    config.out.buffer = frame_buffer;
    config.out.buffer_size = frame_bytes_;
    config.out.pic_w = width_;
    config.out.pic_h = height_;
    config.out.block_offset_x = static_cast<uint32_t>(region.x);
    config.out.block_offset_y = static_cast<uint32_t>(region.y);
    config.out.blend_cm = PPA_BLEND_COLOR_MODE_RGB888;
    config.bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    config.bg_alpha_fix_val = UINT8_MAX;
    config.fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    config.fg_fix_rgb_val = {
        .b = static_cast<uint8_t>(status_scrim_rgb_),
        .g = static_cast<uint8_t>(status_scrim_rgb_ >> 8U),
        .r = static_cast<uint8_t>(status_scrim_rgb_ >> 16U),
    };
    config.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t status = ppa_do_blend(blend_client_, &config);
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "PPA status scrim blend failed: %s", esp_err_to_name(status));
        return false;
    }
    return true;
}

bool SystemTransitionCompositor::CopyStatusScrimRegion(uint8_t* frame_buffer, const SystemTransitionRect& region) {
    if (status_scrim_background_pixels_ == nullptr || frame_buffer == nullptr || region.x < 0 || region.y < 0 ||
        region.width <= 0 || region.height <= 0 || region.x + region.width > static_cast<int32_t>(width_) ||
        region.y + region.height > static_cast<int32_t>(height_)) {
        return false;
    }
    return CopyRgb888(status_scrim_background_pixels_, width_, height_, static_cast<uint32_t>(region.x),
                      static_cast<uint32_t>(region.y), frame_buffer, width_, height_, static_cast<uint32_t>(region.x),
                      static_cast<uint32_t>(region.y), static_cast<uint32_t>(region.width),
                      static_cast<uint32_t>(region.height));
}

bool SystemTransitionCompositor::CaptureStatusDialogLocked(lv_obj_t* dialog) {
    if (dialog == nullptr) {
        return false;
    }
    lv_obj_update_layout(dialog);
    const int32_t ext_draw_size = lv_obj_get_ext_draw_size(dialog);
    const int32_t width = lv_obj_get_width(dialog) + ext_draw_size * 2;
    const int32_t height = lv_obj_get_height(dialog) + ext_draw_size * 2;
    if (ext_draw_size < 0 || width <= 0 || height <= 0 || width > static_cast<int32_t>(width_) ||
        height > static_cast<int32_t>(height_) || static_cast<uint32_t>(width) > UINT32_MAX / 4U ||
        static_cast<uint32_t>(width) * 4U > UINT32_MAX / static_cast<uint32_t>(height)) {
        return false;
    }
    const uint32_t bytes = static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * 4U;
    const uint32_t allocation_bytes = AlignPpaBufferSize(bytes);
    if (status_dialog_pixels_ == nullptr || status_dialog_allocation_bytes_ < allocation_bytes) {
        heap_caps_free(status_dialog_pixels_);
        status_dialog_pixels_ = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(kPpaBufferAlignment, allocation_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        status_dialog_allocation_bytes_ = status_dialog_pixels_ != nullptr ? allocation_bytes : 0U;
    }
    if (status_dialog_pixels_ == nullptr) {
        ESP_LOGE(kTag, "could not allocate status dialog snapshot: bytes=%" PRIu32, allocation_bytes);
        return false;
    }
    std::memset(status_dialog_pixels_, 0, bytes);
    lv_draw_buf_t snapshot{};
    const uint32_t stride = static_cast<uint32_t>(width) * 4U;
    const bool captured =
        lv_draw_buf_init(&snapshot, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                         LV_COLOR_FORMAT_ARGB8888, stride, status_dialog_pixels_, bytes) == LV_RESULT_OK &&
        lv_snapshot_take_to_draw_buf(dialog, LV_COLOR_FORMAT_ARGB8888, &snapshot) == LV_RESULT_OK;
    if (!captured || snapshot.header.w != static_cast<uint32_t>(width) ||
        snapshot.header.h != static_cast<uint32_t>(height) || snapshot.header.stride != stride) {
        ESP_LOGE(kTag, "could not render status dialog snapshot");
        return false;
    }
    lv_draw_buf_flush_cache(&snapshot, nullptr);
    status_dialog_width_ = static_cast<uint32_t>(width);
    status_dialog_height_ = static_cast<uint32_t>(height);
    status_dialog_x_ = lv_obj_get_x(dialog) - ext_draw_size;
    status_dialog_ext_draw_size_ = ext_draw_size;
    return true;
}

SystemTransitionRect SystemTransitionCompositor::VisibleStatusDialogRect(int32_t x, int32_t y) const {
    const int32_t left = std::max<int32_t>(0, x);
    const int32_t top = std::max<int32_t>(0, y);
    const int32_t right =
        std::min<int32_t>(static_cast<int32_t>(width_), x + static_cast<int32_t>(status_dialog_width_));
    const int32_t bottom =
        std::min<int32_t>(static_cast<int32_t>(height_), y + static_cast<int32_t>(status_dialog_height_));
    return right > left && bottom > top
               ? SystemTransitionRect{.x = left, .y = top, .width = right - left, .height = bottom - top}
               : SystemTransitionRect{};
}

bool SystemTransitionCompositor::ComposeStatusDialog(uint8_t* frame_buffer, const SystemTransitionRect& region) {
    if (blend_client_ == nullptr || status_dialog_pixels_ == nullptr || frame_buffer == nullptr || region.width <= 0 ||
        region.height <= 0) {
        return false;
    }
    const uint32_t source_x = static_cast<uint32_t>(region.x - status_dialog_x_);
    const uint32_t source_y = region.y == 0 && region.height < static_cast<int32_t>(status_dialog_height_)
                                  ? status_dialog_height_ - static_cast<uint32_t>(region.height)
                                  : 0U;
    ppa_blend_oper_config_t config{};
    config.in_bg.buffer = frame_buffer;
    config.in_bg.pic_w = width_;
    config.in_bg.pic_h = height_;
    config.in_bg.block_w = static_cast<uint32_t>(region.width);
    config.in_bg.block_h = static_cast<uint32_t>(region.height);
    config.in_bg.block_offset_x = static_cast<uint32_t>(region.x);
    config.in_bg.block_offset_y = static_cast<uint32_t>(region.y);
    config.in_bg.blend_cm = PPA_BLEND_COLOR_MODE_RGB888;
    config.in_fg.buffer = status_dialog_pixels_;
    config.in_fg.pic_w = status_dialog_width_;
    config.in_fg.pic_h = status_dialog_height_;
    config.in_fg.block_w = static_cast<uint32_t>(region.width);
    config.in_fg.block_h = static_cast<uint32_t>(region.height);
    config.in_fg.block_offset_x = source_x;
    config.in_fg.block_offset_y = source_y;
    config.in_fg.blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888;
    config.out.buffer = frame_buffer;
    config.out.buffer_size = frame_bytes_;
    config.out.pic_w = width_;
    config.out.pic_h = height_;
    config.out.block_offset_x = static_cast<uint32_t>(region.x);
    config.out.block_offset_y = static_cast<uint32_t>(region.y);
    config.out.blend_cm = PPA_BLEND_COLOR_MODE_RGB888;
    config.bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    config.bg_alpha_fix_val = UINT8_MAX;
    config.fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    config.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t status = ppa_do_blend(blend_client_, &config);
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "PPA status dialog blend failed: %s", esp_err_to_name(status));
        return false;
    }
    return true;
}

bool SystemTransitionCompositor::BeginStatusLayerTransition(bool entering, uint32_t scrim_rgb, uint8_t scrim_opacity,
                                                            uint64_t trigger_timestamp_us) {
    if (display_ == nullptr || panel_ == nullptr || blend_client_ == nullptr || prepared_to_hall_ ||
        status_transition_dummy_active_ || (!entering && !status_layer_buffers_ready_)) {
        return false;
    }
    if (entering) {
        ClearStatusLayerBuffers();
        status_background_pixels_ = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(kPpaBufferAlignment, frame_bytes_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        status_scrim_background_pixels_ = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(kPpaBufferAlignment, frame_bytes_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        const uint32_t alpha_bytes = width_ * height_;
        status_scrim_alpha_pixels_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            kPpaBufferAlignment, AlignPpaBufferSize(alpha_bytes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (status_background_pixels_ == nullptr || status_scrim_background_pixels_ == nullptr ||
            status_scrim_alpha_pixels_ == nullptr) {
            ESP_LOGE(kTag, "could not allocate status-layer compositor buffers");
            ClearStatusLayerBuffers();
            return false;
        }
        std::memset(status_scrim_alpha_pixels_, scrim_opacity, alpha_bytes);
        status_scrim_rgb_ = scrim_rgb;
    }
    const esp_err_t dummy_status = esp_lv_adapter_set_dummy_draw(display_, true);
    if (dummy_status != ESP_OK) {
        ESP_LOGE(kTag, "could not freeze display for status-layer transition: %s", esp_err_to_name(dummy_status));
        if (entering) {
            ClearStatusLayerBuffers();
        }
        return false;
    }
    status_transition_dummy_active_ = true;
    status_frame_states_ = {};
    if (!entering) {
        return true;
    }

    uint8_t* displayed_frame_buffer = DisplayedFrameBuffer();
    auto* first_frame = static_cast<uint8_t*>(esp_lv_adapter_dummy_draw_get_free_buf_preserve(display_));
    const SystemTransitionRect fullscreen{
        .x = 0, .y = 0, .width = static_cast<int32_t>(width_), .height = static_cast<int32_t>(height_)};
    const bool captured = displayed_frame_buffer != nullptr &&
                          CopyRgb888(displayed_frame_buffer, width_, height_, 0U, 0U, status_background_pixels_, width_,
                                     height_, 0U, 0U, width_, height_);
    const bool scrim_composed = captured && ComposeStatusScrimRegion(status_scrim_background_pixels_, fullscreen);
    const bool presented = scrim_composed && first_frame != nullptr && CopyStatusScrimRegion(first_frame, fullscreen) &&
                           SubmitFrame(first_frame);
    if (!presented) {
        ESP_LOGE(kTag, "could not present initial hardware status-layer frame");
        CancelStatusLayerTransition();
        return false;
    }
    status_frame_states_[0] = {
        .buffer = first_frame,
        .dialog_region = {},
        .scrim_ready = true,
        .has_dialog = false,
    };
    status_layer_buffers_ready_ = true;
    const int64_t first_present_us = esp_timer_get_time();
    if (trigger_timestamp_us != 0U && first_present_us >= static_cast<int64_t>(trigger_timestamp_us)) {
        ESP_LOGI(kTag,
                 "status transition responsiveness: direction=open phase=scrim trigger-to-first-present=%" PRIu64 " us",
                 static_cast<uint64_t>(first_present_us) - trigger_timestamp_us);
    }
    return true;
}

bool SystemTransitionCompositor::AnimateStatusLayerLocked(lv_obj_t* dialog, int32_t visible_y, int32_t hidden_y,
                                                          bool entering, uint32_t duration_ms,
                                                          uint64_t trigger_timestamp_us) {
    if (dialog == nullptr || !status_transition_dummy_active_ || !status_layer_buffers_ready_ ||
        (entering ? !CaptureStatusDialogLocked(dialog) : status_dialog_pixels_ == nullptr)) {
        return false;
    }
    status_dialog_x_ = lv_obj_get_x(dialog) - status_dialog_ext_draw_size_;
    const int32_t visible_snapshot_y = visible_y - status_dialog_ext_draw_size_;
    const int32_t hidden_snapshot_y = hidden_y - status_dialog_ext_draw_size_;
    constexpr std::array<uint16_t, 6U> kEnterProgress{{400U, 650U, 820U, 930U, 1000U, 1000U}};
    constexpr std::array<uint16_t, 6U> kExitProgress{{950U, 800U, 550U, 250U, 0U, 0U}};
    const auto& frame_progress = entering ? kEnterProgress : kExitProgress;
    const int64_t started_us = esp_timer_get_time();
    int64_t first_present_us = 0;
    bool success = true;

    for (uint32_t frame = 0U; frame < frame_progress.size(); ++frame) {
        const int64_t frame_started_us = esp_timer_get_time();
        auto* frame_buffer = static_cast<uint8_t*>(esp_lv_adapter_dummy_draw_get_free_buf_preserve(display_));
        StatusFrameState* frame_state = nullptr;
        for (auto& candidate : status_frame_states_) {
            if (candidate.buffer == frame_buffer || (candidate.buffer == nullptr && frame_state == nullptr)) {
                frame_state = &candidate;
                if (candidate.buffer == frame_buffer) {
                    break;
                }
            }
        }
        if (frame_state == nullptr || frame_buffer == nullptr) {
            success = false;
            break;
        }
        if (frame_state->buffer == nullptr) {
            frame_state->buffer = frame_buffer;
            frame_state->scrim_ready = !entering;
            frame_state->has_dialog = !entering;
            frame_state->dialog_region = VisibleStatusDialogRect(status_dialog_x_, visible_snapshot_y);
        }

        const uint16_t progress = frame_progress[frame];
        const int32_t dialog_y =
            hidden_snapshot_y +
            static_cast<int32_t>(static_cast<int64_t>(visible_snapshot_y - hidden_snapshot_y) * progress / 1000);
        const SystemTransitionRect new_region = VisibleStatusDialogRect(status_dialog_x_, dialog_y);
        if (!entering && progress == 0U) {
            success = CopyRgb888(status_background_pixels_, width_, height_, 0U, 0U, frame_buffer, width_, height_, 0U,
                                 0U, width_, height_);
            frame_state->scrim_ready = false;
            frame_state->has_dialog = false;
            frame_state->dialog_region = {};
        } else {
            if (!frame_state->scrim_ready) {
                const SystemTransitionRect fullscreen{
                    .x = 0, .y = 0, .width = static_cast<int32_t>(width_), .height = static_cast<int32_t>(height_)};
                success = CopyStatusScrimRegion(frame_buffer, fullscreen);
                frame_state->scrim_ready = success;
            } else if (frame_state->has_dialog) {
                success = CopyStatusScrimRegion(frame_buffer, frame_state->dialog_region);
            }
            if (success && new_region.width > 0 && new_region.height > 0) {
                success = ComposeStatusDialog(frame_buffer, new_region);
            }
            if (success) {
                frame_state->has_dialog = new_region.width > 0 && new_region.height > 0;
                frame_state->dialog_region = new_region;
            }
        }
        success = success && SubmitFrame(frame_buffer);
        if (!success) {
            break;
        }
        if (first_present_us == 0) {
            first_present_us = esp_timer_get_time();
        }
        ESP_LOGD(kTag, "status PPA frame: direction=%s frame=%" PRIu32 " progress=%u total=%" PRIu32 " us",
                 entering ? "open" : "close", frame, static_cast<unsigned>(progress),
                 static_cast<uint32_t>(esp_timer_get_time() - frame_started_us));
    }
    const uint32_t elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
    ESP_LOGI(kTag,
             "status PPA transition complete: direction=%s frames=%u elapsed=%" PRIu32 " us target=%" PRIu32
             " ms status=%s",
             entering ? "open" : "close", static_cast<unsigned>(frame_progress.size()), elapsed_us, duration_ms,
             success ? "ok" : "failed");
    if (!entering && trigger_timestamp_us != 0U && first_present_us >= static_cast<int64_t>(trigger_timestamp_us)) {
        ESP_LOGI(kTag, "status transition responsiveness: direction=close trigger-to-first-present=%" PRIu64 " us",
                 static_cast<uint64_t>(first_present_us) - trigger_timestamp_us);
    }
    return success;
}

bool SystemTransitionCompositor::FinishStatusLayerTransition(bool keep_buffers) {
    if (!status_transition_dummy_active_) {
        return false;
    }
    const esp_err_t status = esp_lv_adapter_disable_dummy_draw_preserve_content(display_);
    status_transition_dummy_active_ = false;
    status_frame_states_ = {};
    if (!keep_buffers) {
        ClearStatusLayerBuffers();
    }
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "could not leave hardware status-layer compositor: %s", esp_err_to_name(status));
        return false;
    }
    return true;
}

void SystemTransitionCompositor::CancelStatusLayerTransition() {
    if (status_transition_dummy_active_ && display_ != nullptr) {
        const esp_err_t status = esp_lv_adapter_set_dummy_draw(display_, false);
        if (status != ESP_OK) {
            ESP_LOGW(kTag, "could not cancel hardware status-layer compositor: %s", esp_err_to_name(status));
        }
    }
    status_transition_dummy_active_ = false;
    ClearStatusLayerBuffers();
}

void SystemTransitionCompositor::ClearStatusLayerBuffers() {
    heap_caps_free(status_dialog_pixels_);
    heap_caps_free(status_scrim_alpha_pixels_);
    heap_caps_free(status_scrim_background_pixels_);
    heap_caps_free(status_background_pixels_);
    status_dialog_pixels_ = nullptr;
    status_scrim_alpha_pixels_ = nullptr;
    status_scrim_background_pixels_ = nullptr;
    status_background_pixels_ = nullptr;
    status_dialog_width_ = 0U;
    status_dialog_height_ = 0U;
    status_dialog_allocation_bytes_ = 0U;
    status_dialog_x_ = 0;
    status_dialog_ext_draw_size_ = 0;
    status_layer_buffers_ready_ = false;
    status_frame_states_ = {};
}

bool SystemTransitionCompositor::Animate(const uint8_t* half_guest, const SystemTransitionRect& card,
                                         SystemTransitionDirection direction, uint32_t duration_ms,
                                         uint64_t trigger_timestamp_us) {
    if (display_ == nullptr || half_guest == nullptr || background_pixels_ == nullptr || card.width != kCardWidth ||
        card.height != kCardWidth) {
        return false;
    }

    const bool continuing_prepared_to_hall = direction == SystemTransitionDirection::kToHall && prepared_to_hall_;
    esp_err_t status = continuing_prepared_to_hall ? ESP_OK : esp_lv_adapter_set_dummy_draw(display_, true);
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "could not enter transition compositor mode: %s", esp_err_to_name(status));
        return false;
    }

    struct FrameBufferState final {
        uint8_t* buffer{};
        SystemTransitionRect guest_region{};
        bool has_guest{};
    };
    std::array<FrameBufferState, 3U> frame_buffers{};

    constexpr uint32_t kFirstVisibleScaleUnits = 11U;
    constexpr uint32_t kLastVisibleScaleUnits = 30U;
    constexpr uint32_t kToHallFirstScaleUnits = 25U;
    constexpr uint32_t kToHallAnimatedFrameCount = 6U;
    constexpr uint32_t kToGuestAnimatedFrameCount = 6U;
    constexpr uint32_t kFinalFramebufferCount = 2U;
    struct FrameTiming final {
        uint32_t scale_units{};
        uint32_t acquire_us{};
        uint32_t repair_us{};
        uint32_t compose_us{};
        uint32_t submit_us{};
        uint32_t total_us{};
        bool final{};
    };
    std::array<FrameTiming, kToGuestAnimatedFrameCount + kFinalFramebufferCount> frame_timings{};
    uint32_t timing_count = 0U;
    const uint32_t animated_frame_count =
        direction == SystemTransitionDirection::kToHall ? kToHallAnimatedFrameCount : kToGuestAnimatedFrameCount;
    const int64_t started_us = esp_timer_get_time();
    int64_t first_present_us = 0;
    uint32_t submitted_frames = 0U;
    bool success = true;

    for (uint32_t frame = 0U; frame < animated_frame_count; ++frame) {
        const uint32_t scale_units =
            direction == SystemTransitionDirection::kToHall
                ? kToHallFirstScaleUnits -
                      (kToHallFirstScaleUnits - kFirstVisibleScaleUnits) * frame / (animated_frame_count - 1U)
                : kFirstVisibleScaleUnits +
                      (kLastVisibleScaleUnits - kFirstVisibleScaleUnits) * frame / (animated_frame_count - 1U);
        FrameTiming& timing = frame_timings[timing_count++];
        timing.scale_units = scale_units;
        const int64_t frame_started_us = esp_timer_get_time();
        int64_t stage_started_us = frame_started_us;
        auto* frame_buffer = static_cast<uint8_t*>(esp_lv_adapter_dummy_draw_get_free_buf_preserve(display_));
        timing.acquire_us = static_cast<uint32_t>(esp_timer_get_time() - stage_started_us);
        FrameBufferState* frame_state = nullptr;
        for (auto& candidate : frame_buffers) {
            if (candidate.buffer == frame_buffer || (candidate.buffer == nullptr && frame_state == nullptr)) {
                frame_state = &candidate;
                if (candidate.buffer == frame_buffer) {
                    break;
                }
            }
        }
        if (frame_state != nullptr && frame_state->buffer == nullptr) {
            frame_state->buffer = frame_buffer;
            // Before the transition both physical buffers already contain the
            // same stable endpoint. Model that endpoint instead of copying a
            // 1.5 MB Hall background into each buffer up front.
            frame_state->has_guest = direction == SystemTransitionDirection::kToHall;
            frame_state->guest_region = {
                .x = 0, .y = 0, .width = static_cast<int32_t>(width_), .height = static_cast<int32_t>(height_)};
        }

        const SystemTransitionRect composed_region = GuestRect(card, scale_units);
        stage_started_us = esp_timer_get_time();
        bool background_repaired = frame_state != nullptr;
        if (background_repaired && frame_state->has_guest) {
            background_repaired = RestoreGuestDifference(frame_buffer, frame_state->guest_region, composed_region);
        }
        timing.repair_us = static_cast<uint32_t>(esp_timer_get_time() - stage_started_us);
        SystemTransitionRect rendered_region{};
        stage_started_us = esp_timer_get_time();
        const bool composed = frame_buffer != nullptr && background_repaired &&
                              ComposeGuest(frame_buffer, half_guest, card, scale_units, rendered_region);
        timing.compose_us = static_cast<uint32_t>(esp_timer_get_time() - stage_started_us);
        stage_started_us = esp_timer_get_time();
        const bool submitted = composed && SubmitFrame(frame_buffer);
        timing.submit_us = static_cast<uint32_t>(esp_timer_get_time() - stage_started_us);
        timing.total_us = static_cast<uint32_t>(esp_timer_get_time() - frame_started_us);
        if (!submitted) {
            success = false;
            break;
        }
        frame_state->has_guest = true;
        frame_state->guest_region = rendered_region;
        ++submitted_frames;
        if (first_present_us == 0) {
            first_present_us = esp_timer_get_time();
        }
    }

    // Preserve-content mode is safe only when every panel framebuffer is back
    // in the same untransformed LVGL coordinate space. Publish the exact final
    // image twice so both buffers are repaired before LVGL owns them again.
    for (uint32_t frame = 0U; success && frame < kFinalFramebufferCount; ++frame) {
        FrameTiming& timing = frame_timings[timing_count++];
        timing.scale_units = direction == SystemTransitionDirection::kToHall ? 9U : 32U;
        timing.final = true;
        const int64_t frame_started_us = esp_timer_get_time();
        int64_t stage_started_us = frame_started_us;
        auto* frame_buffer = static_cast<uint8_t*>(esp_lv_adapter_dummy_draw_get_free_buf_preserve(display_));
        timing.acquire_us = static_cast<uint32_t>(esp_timer_get_time() - stage_started_us);
        FrameBufferState* frame_state = nullptr;
        for (auto& candidate : frame_buffers) {
            if (candidate.buffer == frame_buffer) {
                frame_state = &candidate;
                break;
            }
        }
        stage_started_us = esp_timer_get_time();
        bool composed = false;
        if (direction == SystemTransitionDirection::kToHall) {
            composed = frame_state != nullptr && frame_state->has_guest
                           ? ComposeBackgroundRegion(frame_buffer, frame_state->guest_region)
                           : ComposeBackground(frame_buffer);
            timing.repair_us = static_cast<uint32_t>(esp_timer_get_time() - stage_started_us);
        } else {
            composed = ScaleRgb888(half_guest, kHalfWidth, kHalfWidth, frame_buffer, width_, height_, frame_bytes_, 0U,
                                   0U, 2.0F);
            timing.compose_us = static_cast<uint32_t>(esp_timer_get_time() - stage_started_us);
        }
        stage_started_us = esp_timer_get_time();
        success = frame_buffer != nullptr && composed && SubmitFrame(frame_buffer);
        timing.submit_us = static_cast<uint32_t>(esp_timer_get_time() - stage_started_us);
        timing.total_us = static_cast<uint32_t>(esp_timer_get_time() - frame_started_us);
        if (success && frame_state != nullptr) {
            frame_state->has_guest = direction == SystemTransitionDirection::kToGuest;
        }
        submitted_frames += success ? 1U : 0U;
    }

    status = success ? esp_lv_adapter_disable_dummy_draw_preserve_content(display_)
                     : esp_lv_adapter_set_dummy_draw(display_, false);
    prepared_to_hall_ = false;
    success = success && status == ESP_OK;
    const uint32_t elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
    ESP_LOGI(kTag,
             "PPA transition complete: direction=%s frames=%" PRIu32 " elapsed=%" PRIu32 " us target=%" PRIu32
             " ms status=%s",
             direction == SystemTransitionDirection::kToHall ? "to-hall" : "to-guest", submitted_frames, elapsed_us,
             duration_ms, success ? "ok" : "failed");
    for (uint32_t frame = 0U; frame < timing_count; ++frame) {
        const FrameTiming& timing = frame_timings[frame];
        ESP_LOGD(kTag,
                 "PPA frame timing: direction=%s frame=%" PRIu32 " phase=%s scale=%" PRIu32 "/16 acquire=%" PRIu32
                 " us repair=%" PRIu32 " us compose=%" PRIu32 " us submit=%" PRIu32 " us total=%" PRIu32 " us",
                 direction == SystemTransitionDirection::kToHall ? "to-hall" : "to-guest", frame,
                 timing.final ? "final" : "animated", timing.scale_units, timing.acquire_us, timing.repair_us,
                 timing.compose_us, timing.submit_us, timing.total_us);
    }
    if (trigger_timestamp_us != 0U && first_present_us >= static_cast<int64_t>(trigger_timestamp_us)) {
        if (continuing_prepared_to_hall) {
            ESP_LOGI(kTag,
                     "transition responsiveness: direction=to-hall phase=continuation trigger-to-compositor=%" PRIu64
                     " us trigger-to-next-present=%" PRIu64 " us",
                     static_cast<uint64_t>(started_us) - trigger_timestamp_us,
                     static_cast<uint64_t>(first_present_us) - trigger_timestamp_us);
        } else {
            ESP_LOGI(kTag,
                     "transition responsiveness: direction=%s trigger-to-compositor=%" PRIu64
                     " us trigger-to-first-present=%" PRIu64 " us",
                     direction == SystemTransitionDirection::kToHall ? "to-hall" : "to-guest",
                     static_cast<uint64_t>(started_us) - trigger_timestamp_us,
                     static_cast<uint64_t>(first_present_us) - trigger_timestamp_us);
        }
    }
    if (!success) {
        ClearBackground();
    }
    return success;
}

void SystemTransitionCompositor::CancelPreparedToHall() {
    if (!prepared_to_hall_) {
        return;
    }
    prepared_to_hall_ = false;
    if (display_ != nullptr) {
        const esp_err_t status = esp_lv_adapter_set_dummy_draw(display_, false);
        if (status != ESP_OK) {
            ESP_LOGW(kTag, "could not cancel prepared Hall transition: %s", esp_err_to_name(status));
        }
    }
}

void SystemTransitionCompositor::ClearBackground() {
    heap_caps_free(background_pixels_);
    heap_caps_free(baseline_background_pixels_);
    background_pixels_ = nullptr;
    baseline_background_pixels_ = nullptr;
}

void SystemTransitionCompositor::Release() {
    CancelStatusLayerTransition();
    CancelPreparedToHall();
    ClearBackground();
    if (dma2d_client_ != nullptr) {
        const esp_err_t status = esp_async_color_convert_uninstall(dma2d_client_);
        if (status != ESP_OK) {
            ESP_LOGW(kTag, "could not release DMA2D background copier: %s", esp_err_to_name(status));
        }
        dma2d_client_ = nullptr;
    }
    if (blend_client_ != nullptr) {
        const esp_err_t status = ppa_unregister_client(blend_client_);
        if (status != ESP_OK) {
            ESP_LOGW(kTag, "could not release PPA status-layer blender: %s", esp_err_to_name(status));
        }
        blend_client_ = nullptr;
    }
    if (srm_client_ != nullptr) {
        const esp_err_t status = ppa_unregister_client(srm_client_);
        if (status != ESP_OK) {
            ESP_LOGW(kTag, "could not release PPA SRM client: %s", esp_err_to_name(status));
        }
        srm_client_ = nullptr;
    }
    panel_ = nullptr;
    display_ = nullptr;
    width_ = 0U;
    height_ = 0U;
    frame_bytes_ = 0U;
}

}  // namespace micropixel::platform::metalio_claw4
