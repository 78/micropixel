#include "platform/metalio-claw4/display/retained_surface.hpp"

#include <cinttypes>
#include <cstddef>
#include <cstdint>

#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "micropixel_display";
constexpr size_t kDmaAlignment = 64U;

uint32_t DurationSample(int64_t duration_us) {
    if (duration_us <= 0) {
        return 0U;
    }
    if (static_cast<uint64_t>(duration_us) > UINT32_MAX) {
        return UINT32_MAX;
    }
    return static_cast<uint32_t>(duration_us);
}

}  // namespace

void RetainedSurface::Bind(lv_display_t* display, esp_lcd_panel_handle_t panel, uint32_t display_width,
                           uint32_t display_height) {
    display_ = display;
    panel_ = panel;
    display_width_ = display_width;
    display_height_ = display_height;
}

void RetainedSurface::Release() {
    if (active_ && display_ != nullptr) {
        (void)esp_lv_adapter_set_dummy_draw(display_, false);
    }
    if (dma2d_client_ != nullptr) {
        (void)esp_async_color_convert_uninstall(dma2d_client_);
    }
    if (fill_client_ != nullptr) {
        (void)ppa_unregister_client(fill_client_);
    }
    heap_caps_free(pixels_);

    lv_display_t* bound_display = display_;
    esp_lcd_panel_handle_t bound_panel = panel_;
    const uint32_t bound_width = display_width_;
    const uint32_t bound_height = display_height_;
    *this = {};
    Bind(bound_display, bound_panel, bound_width, bound_height);
}

bool RetainedSurface::Configure(lv_obj_t* root, const micropixel_graphics_begin_surface_command_t& command,
                                bool background_valid, uint32_t background_rgb888) {
    if (configured_) {
        return x_ == command.x && y_ == command.y && width_ == command.width && height_ == command.height;
    }
    if (display_ == nullptr || panel_ == nullptr) {
        return false;
    }

    const uint32_t stride = static_cast<uint32_t>(command.width) * 3U;
    const size_t surface_bytes = static_cast<size_t>(stride) * static_cast<uint32_t>(command.height);
    if (surface_bytes == 0U || surface_bytes > UINT32_MAX) {
        return false;
    }
    pixels_ = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(kDmaAlignment, surface_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels_ == nullptr) {
        Release();
        ESP_LOGE(kTag, "failed to allocate retained surface pixels");
        return false;
    }
    async_color_convert_config_t dma2d_config{};
    dma2d_config.backlog = 1U;
    dma2d_config.dma_burst_size = 128U;
    esp_err_t status = esp_async_color_convert_install_dma2d(&dma2d_config, &dma2d_client_);
    if (status != ESP_OK) {
        Release();
        ESP_LOGE(kTag, "failed to install retained-surface DMA2D client: %s", esp_err_to_name(status));
        return false;
    }
    ppa_client_config_t fill_config{};
    fill_config.oper_type = PPA_OPERATION_FILL;
    fill_config.max_pending_trans_num = 1U;
    fill_config.data_burst_length = PPA_DATA_BURST_LENGTH_128;
    status = ppa_register_client(&fill_config, &fill_client_);
    if (status != ESP_OK) {
        Release();
        ESP_LOGE(kTag, "failed to install retained-surface PPA fill client: %s", esp_err_to_name(status));
        return false;
    }

    background_rgb888_ = background_valid ? background_rgb888 : 0U;
    frame_ = lv_obj_create(root);
    lv_obj_set_pos(frame_, command.x, command.y);
    lv_obj_set_size(frame_, command.width, command.height);
    lv_obj_set_style_pad_all(frame_, 0, 0);
    lv_obj_set_style_border_width(frame_, 0, 0);
    lv_obj_set_style_radius(frame_, 0, 0);
    lv_obj_set_style_bg_color(frame_, lv_color_hex(background_rgb888_), 0);
    lv_obj_set_style_bg_opa(frame_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(frame_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(frame_, LV_OBJ_FLAG_CLICKABLE);

    surface_bytes_ = static_cast<uint32_t>(surface_bytes);
    x_ = command.x;
    y_ = command.y;
    width_ = command.width;
    height_ = command.height;
    configured_ = true;
    ESP_LOGI(kTag,
             "Host-owned surface ready: %" PRId32 "x%" PRId32
             " RGB888 cache=%zu bytes PSRAM compositor=front-buffer-dma2d+ppa",
             command.width, command.height, surface_bytes);
    return true;
}

void RetainedSurface::SetBackground(uint32_t rgb888) {
    background_rgb888_ = rgb888;
    if (configured_) {
        lv_obj_set_style_bg_color(frame_, lv_color_hex(rgb888), 0);
    }
}

esp_err_t RetainedSurface::CopyRgb888(const uint8_t* source, uint32_t source_width, uint32_t source_height,
                                      uint32_t source_x, uint32_t source_y, uint8_t* destination,
                                      uint32_t destination_width, uint32_t destination_height, uint32_t destination_x,
                                      uint32_t destination_y, uint32_t width, uint32_t height) {
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
    copy.copy_width = width;
    copy.copy_height = height;
    copy.src_color_format = ESP_COLOR_FOURCC_BGR24;
    copy.dst_color_format = ESP_COLOR_FOURCC_BGR24;
    return esp_color_convert_blocking(dma2d_client_, &copy, -1);
}

esp_err_t RetainedSurface::RestoreRect(uint8_t* destination, const SurfaceRect& rect) {
    if (rect.width <= 0 || rect.height <= 0) {
        return ESP_OK;
    }
    ppa_fill_oper_config_t fill{};
    fill.out.buffer = destination;
    fill.out.buffer_size = static_cast<size_t>(display_width_) * display_height_ * 3U;
    fill.out.pic_w = display_width_;
    fill.out.pic_h = display_height_;
    fill.out.block_offset_x = rect.x;
    fill.out.block_offset_y = rect.y;
    fill.out.fill_cm = PPA_FILL_COLOR_MODE_RGB888;
    fill.fill_block_w = rect.width;
    fill.fill_block_h = rect.height;
    fill.fill_argb_color.val = 0xff000000U | background_rgb888_;
    fill.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_fill(fill_client_, &fill);
}

esp_err_t RetainedSurface::RestoreExposed(uint8_t* destination, int32_t old_translate_x, int32_t old_translate_y,
                                          int32_t new_translate_x, int32_t new_translate_y) {
    const SurfaceRect old_rect{x_ + old_translate_x, y_ + old_translate_y, width_, height_};
    const SurfaceRect new_rect{x_ + new_translate_x, y_ + new_translate_y, width_, height_};
    const int32_t old_right = old_rect.x + old_rect.width;
    const int32_t old_bottom = old_rect.y + old_rect.height;
    const int32_t new_right = new_rect.x + new_rect.width;
    const int32_t new_bottom = new_rect.y + new_rect.height;
    const int32_t overlap_top = old_rect.y > new_rect.y ? old_rect.y : new_rect.y;
    const int32_t overlap_bottom = old_bottom < new_bottom ? old_bottom : new_bottom;

    const SurfaceRect strips[] = {
        {old_rect.x, old_rect.y, old_rect.width, new_rect.y > old_rect.y ? new_rect.y - old_rect.y : 0},
        {old_rect.x, new_bottom, old_rect.width, old_bottom > new_bottom ? old_bottom - new_bottom : 0},
        {old_rect.x, overlap_top, new_rect.x > old_rect.x ? new_rect.x - old_rect.x : 0, overlap_bottom - overlap_top},
        {new_right, overlap_top, old_right > new_right ? old_right - new_right : 0, overlap_bottom - overlap_top},
    };
    for (const SurfaceRect& strip : strips) {
        esp_err_t status = RestoreRect(destination, strip);
        if (status != ESP_OK) {
            return status;
        }
    }
    return ESP_OK;
}

RetainedSurface::FrameState* RetainedSurface::FindFrameState(uint8_t* frame_buffer) {
    FrameState* empty = nullptr;
    for (FrameState& state : frame_states_) {
        if (state.buffer == frame_buffer) {
            return &state;
        }
        if (empty == nullptr && state.buffer == nullptr) {
            empty = &state;
        }
    }
    if (empty != nullptr) {
        empty->buffer = frame_buffer;
    }
    return empty;
}

bool RetainedSurface::AllFramebuffersRestored() const {
    for (const FrameState& state : frame_states_) {
        if (state.buffer == nullptr || state.generation != generation_ || state.translate_x != 0 ||
            state.translate_y != 0) {
            return false;
        }
    }
    return true;
}

bool RetainedSurface::ComposeFrame(int32_t translate_x, int32_t translate_y, uint8_t* acquired_frame_buffer,
                                   uint32_t& acquire_us, uint32_t& restore_us, uint32_t& surface_copy_us,
                                   uint32_t& flush_us) {
    const int64_t acquire_started_us = esp_timer_get_time();
    auto* frame_buffer = acquired_frame_buffer != nullptr
                             ? acquired_frame_buffer
                             : static_cast<uint8_t*>(esp_lv_adapter_dummy_draw_get_free_buf_preserve(display_));
    acquire_us = DurationSample(esp_timer_get_time() - acquire_started_us);
    if (frame_buffer == nullptr) {
        ESP_LOGE(kTag, "retained surface could not acquire display frame buffer");
        return false;
    }
    FrameState* state = FindFrameState(frame_buffer);
    if (state == nullptr) {
        ESP_LOGE(kTag, "retained surface saw an unexpected display buffer");
        return false;
    }
    if (state->generation != generation_) {
        state->translate_x = 0;
        state->translate_y = 0;
        state->generation = generation_;
    }

    int64_t phase_us = esp_timer_get_time();
    esp_err_t status = RestoreExposed(frame_buffer, state->translate_x, state->translate_y, translate_x, translate_y);
    restore_us = DurationSample(esp_timer_get_time() - phase_us);
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "retained surface restore failed: %s", esp_err_to_name(status));
        return false;
    }

    phase_us = esp_timer_get_time();
    status = CopyRgb888(pixels_, static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 0U, 0U, frame_buffer,
                        display_width_, display_height_, static_cast<uint32_t>(x_ + translate_x),
                        static_cast<uint32_t>(y_ + translate_y), static_cast<uint32_t>(width_),
                        static_cast<uint32_t>(height_));
    surface_copy_us = DurationSample(esp_timer_get_time() - phase_us);
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "retained surface DMA2D composite failed: %s", esp_err_to_name(status));
        return false;
    }
    state->translate_x = translate_x;
    state->translate_y = translate_y;

    phase_us = esp_timer_get_time();
    status = esp_lv_adapter_dummy_draw_flush_buf(display_, frame_buffer);
    flush_us = DurationSample(esp_timer_get_time() - phase_us);
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "retained surface frame submit failed: %s", esp_err_to_name(status));
        return false;
    }
    return true;
}

bool RetainedSurface::Update(const micropixel_graphics_begin_surface_command_t* request) {
    const bool requested_active =
        request != nullptr && (request->flags & MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION_ACTIVE) != 0U;
    if (!requested_active) {
        if (!active_) {
            return false;
        }
        uint32_t cleanup_frames = 0U;
        const int64_t cleanup_started_us = esp_timer_get_time();
        while (!AllFramebuffersRestored() && cleanup_frames < kFramebufferCount) {
            uint32_t acquire_us = 0U;
            uint32_t restore_us = 0U;
            uint32_t surface_copy_us = 0U;
            uint32_t flush_us = 0U;
            if (!ComposeFrame(0, 0, nullptr, acquire_us, restore_us, surface_copy_us, flush_us)) {
                break;
            }
            ++cleanup_frames;
        }
        const uint32_t cleanup_us = DurationSample(esp_timer_get_time() - cleanup_started_us);
        const bool buffers_restored = AllFramebuffersRestored();
        esp_err_t status = buffers_restored ? esp_lv_adapter_disable_dummy_draw_preserve_content(display_)
                                            : esp_lv_adapter_set_dummy_draw(display_, false);
        if (status != ESP_OK && buffers_restored) {
            status = esp_lv_adapter_set_dummy_draw(display_, false);
        }
        active_ = false;
        translate_x_ = 0;
        translate_y_ = 0;
        ESP_LOGI(kTag,
                 "retained surface translation complete: updates=%" PRIu32 " cleanup=%" PRIu32 "/%" PRIu32
                 "us buffers=%s restore=%s",
                 translate_count_, cleanup_frames, cleanup_us, buffers_restored ? "preserved" : "full-refresh",
                 esp_err_to_name(status));
        translate_count_ = 0U;
        return true;
    }

    if (!active_) {
        const int64_t enable_started_us = esp_timer_get_time();
        esp_err_t status = esp_lv_adapter_set_dummy_draw(display_, true);
        const int64_t enable_completed_us = esp_timer_get_time();
        if (status != ESP_OK) {
            ESP_LOGE(kTag, "retained surface compositor enable failed: %s", esp_err_to_name(status));
            return false;
        }

        void* panel_frame_buffers[kFramebufferCount]{};
        status = esp_lcd_dpi_panel_get_frame_buffer(panel_, kFramebufferCount, &panel_frame_buffers[0],
                                                    &panel_frame_buffers[1]);
        auto* initial_frame_buffer = static_cast<uint8_t*>(esp_lv_adapter_dummy_draw_get_free_buf_preserve(display_));
        auto* displayed_frame_buffer = initial_frame_buffer == panel_frame_buffers[0]
                                           ? static_cast<uint8_t*>(panel_frame_buffers[1])
                                           : static_cast<uint8_t*>(panel_frame_buffers[0]);
        if (status != ESP_OK || initial_frame_buffer == nullptr ||
            (initial_frame_buffer != panel_frame_buffers[0] && initial_frame_buffer != panel_frame_buffers[1])) {
            (void)esp_lv_adapter_set_dummy_draw(display_, false);
            ESP_LOGE(kTag, "retained surface could not resolve display buffers: %s", esp_err_to_name(status));
            return false;
        }

        const int64_t surface_started_us = esp_timer_get_time();
        status = CopyRgb888(displayed_frame_buffer, display_width_, display_height_, static_cast<uint32_t>(x_),
                            static_cast<uint32_t>(y_), pixels_, static_cast<uint32_t>(width_),
                            static_cast<uint32_t>(height_), 0U, 0U, static_cast<uint32_t>(width_),
                            static_cast<uint32_t>(height_));
        const int64_t surface_completed_us = esp_timer_get_time();
        if (status != ESP_OK) {
            (void)esp_lv_adapter_set_dummy_draw(display_, false);
            ESP_LOGE(kTag, "retained surface front-buffer capture failed: %s", esp_err_to_name(status));
            return false;
        }

        active_ = true;
        translate_x_ = request->translate_x;
        translate_y_ = request->translate_y;
        ++generation_;
        ++capture_count_;
        uint32_t acquire_us = 0U;
        uint32_t restore_us = 0U;
        uint32_t surface_copy_us = 0U;
        uint32_t flush_us = 0U;
        const bool composed = ComposeFrame(request->translate_x, request->translate_y, initial_frame_buffer, acquire_us,
                                           restore_us, surface_copy_us, flush_us);
        ++translate_count_;
        ESP_LOGI(kTag,
                 "retained surface compositor #%" PRIu32 ": bytes=%" PRIu32 " surface=%" PRId64 " us base=%" PRId64
                 " us enable=%" PRId64 " us acquire=%" PRIu32 " us restore=%" PRIu32 " us layer=%" PRIu32
                 " us flush=%" PRIu32 " us psram-free=%zu",
                 capture_count_, surface_bytes_, surface_completed_us - surface_started_us, static_cast<int64_t>(0),
                 enable_completed_us - enable_started_us, acquire_us, restore_us, surface_copy_us, flush_us,
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        if (!composed) {
            (void)esp_lv_adapter_set_dummy_draw(display_, false);
            active_ = false;
            return false;
        }
        return true;
    }

    const bool offset_changed = translate_x_ != request->translate_x || translate_y_ != request->translate_y;
    const bool finishing_buffer_rotation =
        request->translate_x == 0 && request->translate_y == 0 && !AllFramebuffersRestored();
    if (offset_changed || finishing_buffer_rotation) {
        translate_x_ = request->translate_x;
        translate_y_ = request->translate_y;
        uint32_t acquire_us = 0U;
        uint32_t restore_us = 0U;
        uint32_t surface_copy_us = 0U;
        uint32_t flush_us = 0U;
        if (!ComposeFrame(request->translate_x, request->translate_y, nullptr, acquire_us, restore_us, surface_copy_us,
                          flush_us)) {
            (void)esp_lv_adapter_set_dummy_draw(display_, false);
            active_ = false;
            return false;
        }
        ++translate_count_;
        ESP_LOGD(kTag,
                 "retained surface composite: offset=%" PRId32 ",%" PRId32 " acquire=%" PRIu32 " us restore=%" PRIu32
                 " us layer=%" PRIu32 " us flush=%" PRIu32 " us",
                 request->translate_x, request->translate_y, acquire_us, restore_us, surface_copy_us, flush_us);
        return true;
    }
    return false;
}

}  // namespace micropixel::platform::metalio_claw4
