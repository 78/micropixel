#include "platform/boards/esp-mosaico/display/panel_transition_compositor.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstring>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "platform/lvgl/display/system_transition_timeline.hpp"
#include "src/core/lv_obj_draw_private.h"
#include "src/draw/snapshot/lv_snapshot.h"

namespace micropixel::platform::mosaico {
namespace {

constexpr char kTag[] = "mosaico_transition";
constexpr uint32_t kRgb565BytesPerPixel = 2U;
constexpr uint32_t kBufferAlignment = 128U;

constexpr uint32_t AlignBufferBytes(uint32_t bytes) {
    return (bytes + kBufferAlignment - 1U) / kBufferAlignment * kBufferAlignment;
}

int32_t Interpolate(int32_t from, int32_t to, uint32_t numerator, uint32_t denominator) {
    return denominator == 0U ? to
                             : from + static_cast<int32_t>((static_cast<int64_t>(to - from) * numerator) / denominator);
}

bool ValidRect(const PanelTransitionRect& rect, uint32_t width, uint32_t height) {
    return rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0 &&
           rect.x + rect.width <= static_cast<int32_t>(width) && rect.y + rect.height <= static_cast<int32_t>(height);
}

esp_err_t DisableDummyDrawAfterSuccessfulTransition(lv_display_t* display) {
#ifdef ESP_LV_ADAPTER_HAS_DISABLE_DUMMY_DRAW_PRESERVE_CONTENT
    return esp_lv_adapter_disable_dummy_draw_preserve_content(display);
#else
    return esp_lv_adapter_set_dummy_draw(display, false);
#endif
}

}  // namespace

PanelTransitionCompositor::~PanelTransitionCompositor() { Release(); }

esp_err_t PanelTransitionCompositor::Initialize(lv_display_t* display, uint32_t width, uint32_t height,
                                                lvgl::SystemTransitionProfile profile,
                                                async_color_convert_handle_t dma2d_client, uint8_t* displayed_source,
                                                const bool* displayed_source_ready) {
    if (display == nullptr || width == 0U || height == 0U || width != height || profile.intermediate_width == 0U ||
        profile.card_width == 0U || dma2d_client == nullptr || displayed_source == nullptr ||
        displayed_source_ready == nullptr || width > UINT32_MAX / height / kRgb565BytesPerPixel) {
        return ESP_ERR_INVALID_ARG;
    }
    if (srm_blitter_.Ready()) {
        return display_ == display && width_ == width && height_ == height ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(srm_blitter_.Initialize(), kTag, "register PPA SRM client failed");
    ppa_client_config_t blend_config{
        .oper_type = PPA_OPERATION_BLEND,
        .max_pending_trans_num = 1U,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
        .flags = {},
    };
    esp_err_t status = ppa_register_client(&blend_config, &blend_client_);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "PPA status-layer blender unavailable: %s", esp_err_to_name(status));
        blend_client_ = nullptr;
    }
    display_ = display;
    width_ = width;
    height_ = height;
    profile_ = profile;
    dma2d_client_ = dma2d_client;
    displayed_source_ = displayed_source;
    displayed_source_ready_ = displayed_source_ready;
    frame_allocation_bytes_ = AlignBufferBytes(width * height * kRgb565BytesPerPixel);
    native_stage_ = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(kBufferAlignment, frame_allocation_bytes_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    wire_stage_ = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(kBufferAlignment, frame_allocation_bytes_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (native_stage_ == nullptr || wire_stage_ == nullptr) {
        Release();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag,
             "CO5300 direct compositor ready: frame=%" PRIu32 "x%" PRIu32 " intermediate=%" PRIu32
             " transport=PSRAM-direct-QSPI status-layer=%s",
             width_, height_, profile_.intermediate_width, blend_client_ != nullptr ? "ppa-blend" : "static");
    return ESP_OK;
}

bool PanelTransitionCompositor::CaptureDisplayedBackground(const uint8_t* fullscreen) {
    if (fullscreen == nullptr || width_ == 0U || height_ == 0U) {
        return false;
    }
    if (background_ == nullptr) {
        background_ = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(kBufferAlignment, frame_allocation_bytes_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (background_ == nullptr) {
            return false;
        }
    }
    async_color_convert_request_t copy{};
    copy.src_buffer = fullscreen;
    copy.src_stride = width_;
    copy.src_height = height_;
    copy.dst_buffer = background_;
    copy.dst_stride = width_;
    copy.dst_height = height_;
    copy.copy_width = width_;
    copy.copy_height = height_;
    copy.src_color_format = ESP_COLOR_FOURCC_RGB16;
    copy.dst_color_format = ESP_COLOR_FOURCC_RGB16;
    const bool captured = esp_color_convert_blocking(dma2d_client_, &copy, -1) == ESP_OK;
    if (!captured) {
        ReleaseBackground();
    }
    return captured;
}

bool PanelTransitionCompositor::UpdateBackgroundRgb888(const uint8_t* pixels, const PanelTransitionRect& region) {
    if (background_ == nullptr || pixels == nullptr || !ValidRect(region, width_, height_)) {
        return false;
    }
    async_color_convert_request_t copy{};
    copy.src_buffer = pixels;
    copy.src_stride = static_cast<uint32_t>(region.width);
    copy.src_height = static_cast<uint32_t>(region.height);
    copy.dst_buffer = background_;
    copy.dst_stride = width_;
    copy.dst_height = height_;
    copy.dst_x = static_cast<uint32_t>(region.x);
    copy.dst_y = static_cast<uint32_t>(region.y);
    copy.copy_width = static_cast<uint32_t>(region.width);
    copy.copy_height = static_cast<uint32_t>(region.height);
    copy.src_color_format = ESP_COLOR_FOURCC_BGR24;
    copy.dst_color_format = ESP_COLOR_FOURCC_RGB16;
    return esp_color_convert_blocking(dma2d_client_, &copy, -1) == ESP_OK;
}

PanelTransitionRect PanelTransitionCompositor::GuestRect(const PanelTransitionRect& card, uint32_t scale_units) const {
    const uint32_t scaled_size =
        lvgl::SystemTransitionTimeline::ScaledDimension(profile_.intermediate_width, scale_units);
    const uint32_t progress_denominator =
        lvgl::SystemTransitionTimeline::kFullscreenScaleUnits - lvgl::SystemTransitionTimeline::kCardScaleUnits;
    const uint32_t progress_numerator = lvgl::SystemTransitionTimeline::kFullscreenScaleUnits - scale_units;
    const int32_t screen_center = static_cast<int32_t>(width_ / 2U);
    const int32_t card_center_x = card.x + card.width / 2;
    const int32_t card_center_y = card.y + card.height / 2;
    const int32_t center_x = Interpolate(screen_center, card_center_x, progress_numerator, progress_denominator);
    const int32_t center_y = Interpolate(screen_center, card_center_y, progress_numerator, progress_denominator);
    const int32_t x = std::clamp<int32_t>(center_x - static_cast<int32_t>(scaled_size / 2U), 0,
                                          static_cast<int32_t>(width_ - scaled_size));
    const int32_t y = std::clamp<int32_t>(center_y - static_cast<int32_t>(scaled_size / 2U), 0,
                                          static_cast<int32_t>(height_ - scaled_size));
    return {.x = x, .y = y, .width = static_cast<int32_t>(scaled_size), .height = static_cast<int32_t>(scaled_size)};
}

PanelTransitionRect PanelTransitionCompositor::AlignedUnion(const PanelTransitionRect& left,
                                                            const PanelTransitionRect& right) const {
    constexpr int32_t kQspiXAlignment = 4;
    int32_t x1 = std::min(left.x, right.x);
    const int32_t y1 = std::min(left.y, right.y);
    int32_t x2 = std::max(left.x + left.width, right.x + right.width);
    const int32_t y2 = std::max(left.y + left.height, right.y + right.height);
    x1 = std::max<int32_t>(0, x1 / kQspiXAlignment * kQspiXAlignment);
    x2 =
        std::min<int32_t>(static_cast<int32_t>(width_), (x2 + kQspiXAlignment - 1) / kQspiXAlignment * kQspiXAlignment);
    return {.x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1};
}

bool PanelTransitionCompositor::PpaCopy(const uint8_t* source, uint32_t source_width, uint32_t source_height,
                                        const PanelTransitionRect& source_region, ppa_srm_color_mode_t source_mode,
                                        uint8_t* destination, uint32_t destination_width, uint32_t destination_height,
                                        uint32_t destination_allocation_bytes, uint32_t destination_x,
                                        uint32_t destination_y, ppa_srm_color_mode_t destination_mode, float scale) {
    if (!srm_blitter_.Ready() || source == nullptr || destination == nullptr ||
        !ValidRect(source_region, source_width, source_height)) {
        return false;
    }
    const esp_err_t status = srm_blitter_.Blit({
        .source = source,
        .source_width = source_width,
        .source_height = source_height,
        .source_region = {.x = static_cast<uint32_t>(source_region.x),
                          .y = static_cast<uint32_t>(source_region.y),
                          .width = static_cast<uint32_t>(source_region.width),
                          .height = static_cast<uint32_t>(source_region.height)},
        .source_mode = source_mode,
        .destination = destination,
        .destination_width = destination_width,
        .destination_height = destination_height,
        .destination_allocation_bytes = destination_allocation_bytes,
        .destination_x = destination_x,
        .destination_y = destination_y,
        .destination_mode = destination_mode,
        .scale_x = scale,
        .scale_y = scale,
        .input_byte_swap = false,
    });
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "PPA transition copy failed: %s source=%" PRIu32 "x%" PRIu32 " scale=%.4f",
                 esp_err_to_name(status), source_width, source_height, static_cast<double>(scale));
    }
    return status == ESP_OK;
}

bool PanelTransitionCompositor::ComposeStage(const uint8_t* intermediate, const PanelTransitionRect& target,
                                             const PanelTransitionRect& update, uint32_t scale_units,
                                             bool overlay_guest) {
    if (background_ == nullptr || native_stage_ == nullptr || wire_stage_ == nullptr || intermediate == nullptr ||
        !ValidRect(target, width_, height_) || !ValidRect(update, width_, height_)) {
        return false;
    }
    const uint32_t update_width = static_cast<uint32_t>(update.width);
    const uint32_t update_height = static_cast<uint32_t>(update.height);
    const bool background_copied =
        PpaCopy(background_, width_, height_, update, PPA_SRM_COLOR_MODE_RGB565, native_stage_, update_width,
                update_height, frame_allocation_bytes_, 0U, 0U, PPA_SRM_COLOR_MODE_RGB565, 1.0F);
    const PanelTransitionRect source_region{.x = 0,
                                            .y = 0,
                                            .width = static_cast<int32_t>(profile_.intermediate_width),
                                            .height = static_cast<int32_t>(profile_.intermediate_width)};
    const bool composed =
        background_copied &&
        (!overlay_guest || PpaCopy(intermediate, profile_.intermediate_width, profile_.intermediate_width,
                                   source_region, PPA_SRM_COLOR_MODE_RGB565, native_stage_, update_width, update_height,
                                   frame_allocation_bytes_, static_cast<uint32_t>(target.x - update.x),
                                   static_cast<uint32_t>(target.y - update.y), PPA_SRM_COLOR_MODE_RGB565,
                                   static_cast<float>(scale_units) /
                                       static_cast<float>(lvgl::SystemTransitionTimeline::kScaleDenominator)));
    if (!composed) {
        return false;
    }

    // PPA byte_swap is an input transform, not an output-format flag. Applying
    // it while scaling makes the interpolator see byte-swapped RGB565 values
    // and creates persistent cyan/magenta fringes. A separate 1:1 pass keeps
    // interpolation canonical while producing the exact big-endian byte stream
    // consumed by the CO5300, without a CPU pixel loop.
    return srm_blitter_.SwapRgb565Bytes(native_stage_, wire_stage_, update_width, update_height,
                                        frame_allocation_bytes_) == ESP_OK;
}

bool PanelTransitionCompositor::BlitStage(const PanelTransitionRect& update) {
    if (display_ == nullptr || wire_stage_ == nullptr || !ValidRect(update, width_, height_)) {
        return false;
    }
    const esp_err_t status = esp_lv_adapter_dummy_draw_blit(display_, update.x, update.y, update.x + update.width,
                                                            update.y + update.height, wire_stage_, true);
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "CO5300 direct PSRAM blit failed: %s", esp_err_to_name(status));
        return false;
    }
    return SyncDisplayedSource(native_stage_, static_cast<uint32_t>(update.width), static_cast<uint32_t>(update.height),
                               update);
}

bool PanelTransitionCompositor::CaptureDisplayedToIntermediate(const uint8_t* fullscreen, uint8_t* intermediate,
                                                               uint32_t intermediate_allocation_bytes,
                                                               const PanelTransitionRect& card,
                                                               uint64_t trigger_timestamp_us, uint32_t& elapsed_us) {
    elapsed_us = 0U;
    if (fullscreen == nullptr || intermediate == nullptr || background_ == nullptr || prepared_to_hall_ ||
        !ValidRect(card, width_, height_) || card.width != static_cast<int32_t>(profile_.card_width) ||
        card.height != static_cast<int32_t>(profile_.card_width)) {
        return false;
    }
    const int64_t started_us = esp_timer_get_time();
    if (esp_lv_adapter_set_dummy_draw(display_, true) != ESP_OK) {
        return false;
    }
    const PanelTransitionRect fullscreen_region{
        .x = 0, .y = 0, .width = static_cast<int32_t>(width_), .height = static_cast<int32_t>(height_)};
    const bool scaled = PpaCopy(fullscreen, width_, height_, fullscreen_region, PPA_SRM_COLOR_MODE_RGB565, intermediate,
                                profile_.intermediate_width, profile_.intermediate_width, intermediate_allocation_bytes,
                                0U, 0U, PPA_SRM_COLOR_MODE_RGB565, profile_.fullscreen_to_intermediate_scale);
    const PanelTransitionRect initial = GuestRect(card, lvgl::SystemTransitionTimeline::kInitialToHallScaleUnits);
    const bool presented = scaled &&
                           ComposeStage(intermediate, initial, fullscreen_region,
                                        lvgl::SystemTransitionTimeline::kInitialToHallScaleUnits, true) &&
                           BlitStage(fullscreen_region);
    elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
    if (!presented) {
        (void)esp_lv_adapter_set_dummy_draw(display_, false);
        return false;
    }
    prepared_to_hall_ = true;
    if (trigger_timestamp_us != 0U && static_cast<uint64_t>(esp_timer_get_time()) >= trigger_timestamp_us) {
        ESP_LOGI(kTag,
                 "transition responsiveness: direction=to-hall phase=initial scale=%" PRIu32
                 "/16 trigger-to-first-present=%" PRIu64 " us capture+present=%" PRIu32 " us",
                 lvgl::SystemTransitionTimeline::kInitialToHallScaleUnits,
                 static_cast<uint64_t>(esp_timer_get_time()) - trigger_timestamp_us, elapsed_us);
    }
    return true;
}

bool PanelTransitionCompositor::ScaleIntermediateToCoverRgb888(const uint8_t* intermediate, uint8_t* cover,
                                                               uint32_t cover_allocation_bytes) {
    const PanelTransitionRect source_region{.x = 0,
                                            .y = 0,
                                            .width = static_cast<int32_t>(profile_.intermediate_width),
                                            .height = static_cast<int32_t>(profile_.intermediate_width)};
    return PpaCopy(intermediate, profile_.intermediate_width, profile_.intermediate_width, source_region,
                   PPA_SRM_COLOR_MODE_RGB565, cover, profile_.card_width, profile_.card_width, cover_allocation_bytes,
                   0U, 0U, PPA_SRM_COLOR_MODE_RGB888, profile_.intermediate_to_card_scale);
}

bool PanelTransitionCompositor::Animate(const uint8_t* intermediate, const PanelTransitionRect& card,
                                        PanelTransitionDirection direction, uint32_t duration_ms,
                                        uint64_t trigger_timestamp_us) {
    if (display_ == nullptr || background_ == nullptr || intermediate == nullptr || !ValidRect(card, width_, height_) ||
        card.width != static_cast<int32_t>(profile_.card_width) ||
        card.height != static_cast<int32_t>(profile_.card_width)) {
        return false;
    }
    const bool continuing_to_hall = direction == PanelTransitionDirection::kToHall && prepared_to_hall_;
    if (!continuing_to_hall && esp_lv_adapter_set_dummy_draw(display_, true) != ESP_OK) {
        return false;
    }

    const int64_t started_us = esp_timer_get_time();
    int64_t first_present_us = 0;
    PanelTransitionRect previous =
        GuestRect(card, direction == PanelTransitionDirection::kToHall
                            ? (continuing_to_hall ? lvgl::SystemTransitionTimeline::kInitialToHallScaleUnits
                                                  : lvgl::SystemTransitionTimeline::kFullscreenScaleUnits)
                            : lvgl::SystemTransitionTimeline::kCardScaleUnits);
    bool success = true;
    for (uint32_t frame = 0U; frame < lvgl::SystemTransitionTimeline::kAnimatedFrameCount; ++frame) {
        const uint32_t scale_units =
            lvgl::SystemTransitionTimeline::AnimatedScaleUnits(direction == PanelTransitionDirection::kToHall, frame);
        const PanelTransitionRect target = GuestRect(card, scale_units);
        const PanelTransitionRect update = AlignedUnion(previous, target);
        success = ComposeStage(intermediate, target, update, scale_units, true) && BlitStage(update);
        if (!success) {
            break;
        }
        if (first_present_us == 0) {
            first_present_us = esp_timer_get_time();
        }
        previous = target;
    }

    if (success) {
        const PanelTransitionRect final =
            direction == PanelTransitionDirection::kToHall
                ? card
                : PanelTransitionRect{
                      .x = 0, .y = 0, .width = static_cast<int32_t>(width_), .height = static_cast<int32_t>(height_)};
        const PanelTransitionRect update = AlignedUnion(previous, final);
        const uint32_t final_scale = direction == PanelTransitionDirection::kToHall
                                         ? lvgl::SystemTransitionTimeline::kCardScaleUnits
                                         : lvgl::SystemTransitionTimeline::kFullscreenScaleUnits;
        // The Hall baseline already contains the exact rounded cover. At the
        // opposite endpoint, scaling the retained intermediate to 32/16 gives
        // the same full-screen convergence used by the P4 compositor.
        success =
            ComposeStage(intermediate, final, update, final_scale, direction == PanelTransitionDirection::kToGuest) &&
            BlitStage(update);
    }

    success = Finish(success);
    const uint32_t elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
    ESP_LOGI(kTag,
             "PPA transition complete: direction=%s frames=%" PRIu32 " elapsed=%" PRIu32 " us target=%" PRIu32
             " ms status=%s",
             direction == PanelTransitionDirection::kToHall ? "to-hall" : "to-guest",
             lvgl::SystemTransitionTimeline::kAnimatedFrameCount + 1U, elapsed_us, duration_ms,
             success ? "ok" : "failed");
    if (trigger_timestamp_us != 0U && first_present_us >= static_cast<int64_t>(trigger_timestamp_us)) {
        ESP_LOGI(kTag,
                 "transition responsiveness: direction=%s trigger-to-compositor=%" PRIu64
                 " us trigger-to-next-present=%" PRIu64 " us",
                 direction == PanelTransitionDirection::kToHall ? "to-hall" : "to-guest",
                 static_cast<uint64_t>(started_us) - trigger_timestamp_us,
                 static_cast<uint64_t>(first_present_us) - trigger_timestamp_us);
    }
    return success;
}

bool PanelTransitionCompositor::SyncDisplayedSource(const uint8_t* source, uint32_t source_width,
                                                    uint32_t source_height, const PanelTransitionRect& destination) {
    if (source == nullptr || displayed_source_ == nullptr || dma2d_client_ == nullptr ||
        !ValidRect(destination, width_, height_) || source_width < static_cast<uint32_t>(destination.width) ||
        source_height < static_cast<uint32_t>(destination.height)) {
        return false;
    }
    async_color_convert_request_t copy{};
    copy.src_buffer = source;
    copy.src_stride = source_width;
    copy.src_height = source_height;
    copy.dst_buffer = displayed_source_;
    copy.dst_stride = width_;
    copy.dst_height = height_;
    copy.dst_x = static_cast<uint32_t>(destination.x);
    copy.dst_y = static_cast<uint32_t>(destination.y);
    copy.copy_width = static_cast<uint32_t>(destination.width);
    copy.copy_height = static_cast<uint32_t>(destination.height);
    copy.src_color_format = ESP_COLOR_FOURCC_RGB16;
    copy.dst_color_format = ESP_COLOR_FOURCC_RGB16;
    return esp_color_convert_blocking(dma2d_client_, &copy, -1) == ESP_OK;
}

bool PanelTransitionCompositor::ComposeStatusScrim() {
    if (blend_client_ == nullptr || status_background_ == nullptr || status_scrim_ == nullptr ||
        status_scrim_alpha_ == nullptr) {
        return false;
    }
    ppa_blend_oper_config_t config{};
    config.in_bg.buffer = status_background_;
    config.in_bg.pic_w = width_;
    config.in_bg.pic_h = height_;
    config.in_bg.block_w = width_;
    config.in_bg.block_h = height_;
    config.in_bg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;
    config.in_fg.buffer = status_scrim_alpha_;
    config.in_fg.pic_w = width_;
    config.in_fg.pic_h = height_;
    config.in_fg.block_w = width_;
    config.in_fg.block_h = height_;
    config.in_fg.blend_cm = PPA_BLEND_COLOR_MODE_A8;
    config.out.buffer = status_scrim_;
    config.out.buffer_size = frame_allocation_bytes_;
    config.out.pic_w = width_;
    config.out.pic_h = height_;
    config.out.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;
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
        ESP_LOGE(kTag, "PPA RGB565 status scrim blend failed: %s", esp_err_to_name(status));
    }
    return status == ESP_OK;
}

bool PanelTransitionCompositor::CaptureStatusDialogLocked(lv_obj_t* dialog) {
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
    const uint32_t allocation_bytes = AlignBufferBytes(bytes);
    if (status_dialog_pixels_ == nullptr || status_dialog_allocation_bytes_ < allocation_bytes) {
        heap_caps_free(status_dialog_pixels_);
        status_dialog_pixels_ = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(kBufferAlignment, allocation_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        status_dialog_allocation_bytes_ = status_dialog_pixels_ != nullptr ? allocation_bytes : 0U;
    }
    if (status_dialog_pixels_ == nullptr) {
        ESP_LOGE(kTag, "could not allocate RGB565 status dialog snapshot: bytes=%" PRIu32, allocation_bytes);
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
        ESP_LOGE(kTag, "could not render RGB565 status dialog snapshot");
        return false;
    }
    lv_draw_buf_flush_cache(&snapshot, nullptr);
    status_dialog_width_ = static_cast<uint32_t>(width);
    status_dialog_height_ = static_cast<uint32_t>(height);
    status_dialog_x_ = lv_obj_get_x(dialog) - ext_draw_size;
    status_dialog_ext_draw_size_ = ext_draw_size;
    return true;
}

PanelTransitionRect PanelTransitionCompositor::VisibleStatusDialogRect(int32_t x, int32_t y) const {
    const int32_t left = std::max<int32_t>(0, x);
    const int32_t top = std::max<int32_t>(0, y);
    const int32_t right =
        std::min<int32_t>(static_cast<int32_t>(width_), x + static_cast<int32_t>(status_dialog_width_));
    const int32_t bottom =
        std::min<int32_t>(static_cast<int32_t>(height_), y + static_cast<int32_t>(status_dialog_height_));
    return right > left && bottom > top
               ? PanelTransitionRect{.x = left, .y = top, .width = right - left, .height = bottom - top}
               : PanelTransitionRect{};
}

bool PanelTransitionCompositor::ComposeStatusStage(const PanelTransitionRect& update,
                                                   const PanelTransitionRect& dialog_region, int32_t dialog_y,
                                                   bool show_dialog, bool restore_background) {
    const uint8_t* base = restore_background ? status_background_ : status_scrim_;
    if (base == nullptr || !ValidRect(update, width_, height_)) {
        return false;
    }
    const uint32_t update_width = static_cast<uint32_t>(update.width);
    const uint32_t update_height = static_cast<uint32_t>(update.height);
    if (!PpaCopy(base, width_, height_, update, PPA_SRM_COLOR_MODE_RGB565, native_stage_, update_width, update_height,
                 frame_allocation_bytes_, 0U, 0U, PPA_SRM_COLOR_MODE_RGB565, 1.0F)) {
        return false;
    }
    if (show_dialog && dialog_region.width > 0 && dialog_region.height > 0) {
        ppa_blend_oper_config_t config{};
        const uint32_t destination_x = static_cast<uint32_t>(dialog_region.x - update.x);
        const uint32_t destination_y = static_cast<uint32_t>(dialog_region.y - update.y);
        config.in_bg.buffer = native_stage_;
        config.in_bg.pic_w = update_width;
        config.in_bg.pic_h = update_height;
        config.in_bg.block_w = static_cast<uint32_t>(dialog_region.width);
        config.in_bg.block_h = static_cast<uint32_t>(dialog_region.height);
        config.in_bg.block_offset_x = destination_x;
        config.in_bg.block_offset_y = destination_y;
        config.in_bg.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;
        config.in_fg.buffer = status_dialog_pixels_;
        config.in_fg.pic_w = status_dialog_width_;
        config.in_fg.pic_h = status_dialog_height_;
        config.in_fg.block_w = static_cast<uint32_t>(dialog_region.width);
        config.in_fg.block_h = static_cast<uint32_t>(dialog_region.height);
        config.in_fg.block_offset_x = static_cast<uint32_t>(dialog_region.x - status_dialog_x_);
        config.in_fg.block_offset_y = static_cast<uint32_t>(dialog_region.y - dialog_y);
        config.in_fg.blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888;
        config.out.buffer = native_stage_;
        config.out.buffer_size = frame_allocation_bytes_;
        config.out.pic_w = update_width;
        config.out.pic_h = update_height;
        config.out.block_offset_x = destination_x;
        config.out.block_offset_y = destination_y;
        config.out.blend_cm = PPA_BLEND_COLOR_MODE_RGB565;
        config.bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
        config.bg_alpha_fix_val = UINT8_MAX;
        config.fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
        config.mode = PPA_TRANS_MODE_BLOCKING;
        const esp_err_t status = ppa_do_blend(blend_client_, &config);
        if (status != ESP_OK) {
            ESP_LOGE(kTag, "PPA RGB565 status dialog blend failed: %s", esp_err_to_name(status));
            return false;
        }
    }
    return srm_blitter_.SwapRgb565Bytes(native_stage_, wire_stage_, update_width, update_height,
                                        frame_allocation_bytes_) == ESP_OK &&
           BlitStage(update);
}

bool PanelTransitionCompositor::BeginStatusLayerTransition(bool entering, uint32_t scrim_rgb, uint8_t scrim_opacity,
                                                           uint64_t trigger_timestamp_us) {
    if (display_ == nullptr || blend_client_ == nullptr || prepared_to_hall_ || status_transition_dummy_active_ ||
        (!entering && !status_layer_buffers_ready_)) {
        return false;
    }
    if (entering) {
        if (displayed_source_ == nullptr || displayed_source_ready_ == nullptr || !*displayed_source_ready_) {
            return false;
        }
        ClearStatusLayerBuffers();
        status_background_ = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(kBufferAlignment, frame_allocation_bytes_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        status_scrim_ = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(kBufferAlignment, frame_allocation_bytes_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        const uint32_t alpha_bytes = width_ * height_;
        status_scrim_alpha_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            kBufferAlignment, AlignBufferBytes(alpha_bytes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (status_background_ == nullptr || status_scrim_ == nullptr || status_scrim_alpha_ == nullptr) {
            ESP_LOGE(kTag, "could not allocate RGB565 status-layer buffers");
            ClearStatusLayerBuffers();
            return false;
        }
        async_color_convert_request_t copy{};
        copy.src_buffer = displayed_source_;
        copy.src_stride = width_;
        copy.src_height = height_;
        copy.dst_buffer = status_background_;
        copy.dst_stride = width_;
        copy.dst_height = height_;
        copy.copy_width = width_;
        copy.copy_height = height_;
        copy.src_color_format = ESP_COLOR_FOURCC_RGB16;
        copy.dst_color_format = ESP_COLOR_FOURCC_RGB16;
        status_scrim_rgb_ = scrim_rgb;
        std::memset(status_scrim_alpha_, scrim_opacity, alpha_bytes);
        if (esp_color_convert_blocking(dma2d_client_, &copy, -1) != ESP_OK || !ComposeStatusScrim()) {
            ClearStatusLayerBuffers();
            return false;
        }
    }
    if (esp_lv_adapter_set_dummy_draw(display_, true) != ESP_OK) {
        if (entering) {
            ClearStatusLayerBuffers();
        }
        return false;
    }
    status_transition_dummy_active_ = true;
    if (!entering) {
        return true;
    }
    const PanelTransitionRect fullscreen{
        .x = 0, .y = 0, .width = static_cast<int32_t>(width_), .height = static_cast<int32_t>(height_)};
    const bool presented =
        PpaCopy(status_scrim_, width_, height_, fullscreen, PPA_SRM_COLOR_MODE_RGB565, native_stage_, width_, height_,
                frame_allocation_bytes_, 0U, 0U, PPA_SRM_COLOR_MODE_RGB565, 1.0F) &&
        srm_blitter_.SwapRgb565Bytes(native_stage_, wire_stage_, width_, height_, frame_allocation_bytes_) == ESP_OK &&
        BlitStage(fullscreen);
    if (!presented) {
        CancelStatusLayerTransition();
        return false;
    }
    status_layer_buffers_ready_ = true;
    const int64_t first_present_us = esp_timer_get_time();
    if (trigger_timestamp_us != 0U && first_present_us >= static_cast<int64_t>(trigger_timestamp_us)) {
        ESP_LOGI(kTag,
                 "status transition responsiveness: direction=open phase=scrim trigger-to-first-present=%" PRIu64 " us",
                 static_cast<uint64_t>(first_present_us) - trigger_timestamp_us);
    }
    return true;
}

bool PanelTransitionCompositor::AnimateStatusLayerLocked(lv_obj_t* dialog, int32_t visible_y, int32_t hidden_y,
                                                         bool entering, uint32_t duration_ms,
                                                         uint64_t trigger_timestamp_us) {
    if (dialog == nullptr || !status_transition_dummy_active_ || !status_layer_buffers_ready_ ||
        (entering ? !CaptureStatusDialogLocked(dialog) : status_dialog_pixels_ == nullptr)) {
        return false;
    }
    status_dialog_x_ = lv_obj_get_x(dialog) - status_dialog_ext_draw_size_;
    const int32_t visible_snapshot_y = visible_y - status_dialog_ext_draw_size_;
    const int32_t hidden_snapshot_y = hidden_y - status_dialog_ext_draw_size_;
    const auto& frame_progress = lvgl::SystemTransitionTimeline::StatusProgress(entering);
    PanelTransitionRect previous =
        entering ? PanelTransitionRect{} : VisibleStatusDialogRect(status_dialog_x_, visible_snapshot_y);
    const int64_t started_us = esp_timer_get_time();
    int64_t first_present_us = 0;
    bool success = true;
    for (uint16_t progress : frame_progress) {
        const int32_t dialog_y =
            hidden_snapshot_y +
            static_cast<int32_t>(static_cast<int64_t>(visible_snapshot_y - hidden_snapshot_y) * progress / 1000);
        const PanelTransitionRect current = VisibleStatusDialogRect(status_dialog_x_, dialog_y);
        if (previous.width == 0 && previous.height == 0 && current.width == 0 && current.height == 0) {
            continue;
        }
        PanelTransitionRect update = previous.width > 0 && previous.height > 0 ? AlignedUnion(previous, current)
                                                                               : AlignedUnion(current, current);
        if (current.width == 0 || current.height == 0) {
            // Match the P4 compositor contract: the final close frame owns
            // the complete retained background, not only the dialog's old
            // bounds. Restoring just that union leaves the scrim everywhere
            // else and forces LVGL to clear it with a visible QSPI wipe.
            update = !entering && progress == 0U ? PanelTransitionRect{.x = 0,
                                                                       .y = 0,
                                                                       .width = static_cast<int32_t>(width_),
                                                                       .height = static_cast<int32_t>(height_)}
                                                 : AlignedUnion(previous, previous);
        }
        success = ComposeStatusStage(update, current, dialog_y, current.width > 0 && current.height > 0,
                                     !entering && progress == 0U);
        if (!success) {
            break;
        }
        if (first_present_us == 0) {
            first_present_us = esp_timer_get_time();
        }
        previous = current;
    }
    ESP_LOGI(kTag,
             "status PPA transition complete: direction=%s frames=%u elapsed=%" PRIu32 " us target=%" PRIu32
             " ms status=%s",
             entering ? "open" : "close", static_cast<unsigned>(frame_progress.size()),
             static_cast<uint32_t>(esp_timer_get_time() - started_us), duration_ms, success ? "ok" : "failed");
    if (!entering && trigger_timestamp_us != 0U && first_present_us >= static_cast<int64_t>(trigger_timestamp_us)) {
        ESP_LOGI(kTag, "status transition responsiveness: direction=close trigger-to-first-present=%" PRIu64 " us",
                 static_cast<uint64_t>(first_present_us) - trigger_timestamp_us);
    }
    return success;
}

bool PanelTransitionCompositor::FinishStatusLayerTransition(bool keep_buffers) {
    if (!status_transition_dummy_active_) {
        return false;
    }
    const esp_err_t status = DisableDummyDrawAfterSuccessfulTransition(display_);
    status_transition_dummy_active_ = false;
    if (!keep_buffers) {
        ClearStatusLayerBuffers();
    }
    return status == ESP_OK;
}

void PanelTransitionCompositor::CancelStatusLayerTransition() {
    if (status_transition_dummy_active_ && display_ != nullptr) {
        const esp_err_t status = esp_lv_adapter_set_dummy_draw(display_, false);
        if (status != ESP_OK) {
            ESP_LOGW(kTag, "could not cancel RGB565 status-layer compositor: %s", esp_err_to_name(status));
        }
    }
    status_transition_dummy_active_ = false;
    ClearStatusLayerBuffers();
}

void PanelTransitionCompositor::ClearStatusLayerBuffers() {
    heap_caps_free(status_dialog_pixels_);
    heap_caps_free(status_scrim_alpha_);
    heap_caps_free(status_scrim_);
    heap_caps_free(status_background_);
    status_dialog_pixels_ = nullptr;
    status_scrim_alpha_ = nullptr;
    status_scrim_ = nullptr;
    status_background_ = nullptr;
    status_dialog_width_ = 0U;
    status_dialog_height_ = 0U;
    status_dialog_allocation_bytes_ = 0U;
    status_dialog_x_ = 0;
    status_dialog_ext_draw_size_ = 0;
    status_layer_buffers_ready_ = false;
}

bool PanelTransitionCompositor::Finish(bool success) {
    const esp_err_t status =
        success ? DisableDummyDrawAfterSuccessfulTransition(display_) : esp_lv_adapter_set_dummy_draw(display_, false);
    prepared_to_hall_ = false;
    return success && status == ESP_OK;
}

void PanelTransitionCompositor::CancelPreparedToHall() {
    if (!prepared_to_hall_) {
        return;
    }
    (void)esp_lv_adapter_set_dummy_draw(display_, false);
    prepared_to_hall_ = false;
}

void PanelTransitionCompositor::ReleaseBackground() {
    heap_caps_free(background_);
    background_ = nullptr;
}

void PanelTransitionCompositor::Release() {
    CancelStatusLayerTransition();
    CancelPreparedToHall();
    ReleaseBackground();
    heap_caps_free(wire_stage_);
    wire_stage_ = nullptr;
    heap_caps_free(native_stage_);
    native_stage_ = nullptr;
    srm_blitter_.Release();
    if (blend_client_ != nullptr) {
        const esp_err_t status = ppa_unregister_client(blend_client_);
        if (status != ESP_OK) {
            ESP_LOGW(kTag, "could not release RGB565 status-layer blender: %s", esp_err_to_name(status));
        }
        blend_client_ = nullptr;
    }
    display_ = nullptr;
    width_ = 0U;
    height_ = 0U;
    frame_allocation_bytes_ = 0U;
    profile_ = {};
    dma2d_client_ = nullptr;
    displayed_source_ = nullptr;
    displayed_source_ready_ = nullptr;
}

}  // namespace micropixel::platform::mosaico
