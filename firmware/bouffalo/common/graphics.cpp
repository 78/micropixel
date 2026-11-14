// SPDX-License-Identifier: Apache-2.0
#include "graphics.hpp"

#include <cstring>
#include <mutex>

#include "media_display.h"
extern "C" {
#include "bflb_mtimer.h"
#include "lcd.h"
#include "lv_port_disp.h"
}
namespace mpx_port {
namespace {
constexpr int32_t ok = MICROPIXEL_STATUS_OK, invalid = MICROPIXEL_STATUS_INVALID_ARGUMENT,
                  oom = MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
struct ReleaseJob {
    d::DirectSurfaceReleaseSink sink;
    uint8_t index;
    SemaphoreHandle_t barrier;
};
g::SurfacePixelFormat Format(uint32_t f, bool swap = false) {
    return f == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? g::SurfacePixelFormat::kBgra8888
           : f == MICROPIXEL_PIXEL_FORMAT_BGR888 ? g::SurfacePixelFormat::kBgr888
           : swap                                ? g::SurfacePixelFormat::kRgb565Swapped
                                                 : g::SurfacePixelFormat::kRgb565;
}
g::ConstPixelSurface Surface(const d::BitmapView& v) {
    return {v.data,   v.size,   v.width,
            v.height, v.stride, Format(v.pixel_format, (v.flags & d::bitmap_flags::kRgb565ByteSwapped) != 0)};
}
g::PixelSurface Target(const d::TextTarget& t) {
    return {t.pixels, t.pitch * t.height,
            t.width,  t.height,
            t.pitch,  Format(MICROPIXEL_PIXEL_FORMAT_RGB565, t.byte_swapped)};
}
g::PixelSurface Acquire() {
    uint32_t pitch{};
    auto* p = media_display_acquire_direct_rgb565(&pitch);
    return {(uint8_t*)p, LCD_W * LCD_H * 2, LCD_W, LCD_H, pitch * 2, g::SurfacePixelFormat::kRgb565};
}
}  // namespace
bool Graphics::Present(g::PixelSurface s) {
    if (!s.pixels) return false;
    if (overlay_) overlay_(s);
    return media_display_present_direct_rgb565((uint16_t*)s.pixels, 0, 0, s.width, s.height);
}
uint32_t Graphics::Width() const { return LCD_W; }
uint32_t Graphics::Height() const { return LCD_H; }
namespace {
uint32_t LvTick() { return static_cast<uint32_t>(bflb_mtimer_get_time_us() / 1000); }
}  // namespace
bool Graphics::Initialize(void (*overlay)(g::PixelSurface)) {
    overlay_ = overlay;
    lv_init();
    lv_tick_set_cb(LvTick);
    lv_port_disp_init();
    if (!lv_display_get_default()) return false;
    releases_ = xQueueCreate(4, sizeof(ReleaseJob));
    if (!releases_) return false;
    if (xTaskCreate(ReleaseWorker, "mpx_release", 1024, this, 4, &release_task_) != pdPASS) return false;
    ready_ = media_display_begin_direct_rgb565();
#if defined(LCD_BACKLIGHT_EN) && LCD_BACKLIGHT_EN
    if (ready_) lcd_backlight_toggle(true);
#endif
    return ready_;
}
void Graphics::ReleaseWorker(void* p) {
    auto* self = (Graphics*)p;
    ReleaseJob job{};
    for (;;)
        if (xQueueReceive(self->releases_, &job, portMAX_DELAY) == pdTRUE) {
            if (job.sink.release) job.sink.release(job.sink.context, job.index, bflb_mtimer_get_time_us());
            if (job.barrier) xSemaphoreGive(job.barrier);
        }
}
void Graphics::Drain() {
    StaticSemaphore_t s;
    auto barrier = xSemaphoreCreateBinaryStatic(&s);
    ReleaseJob job{};
    job.barrier = barrier;
    xQueueSend(releases_, &job, portMAX_DELAY);
    xSemaphoreTake(barrier, portMAX_DELAY);
}
int32_t Graphics::GetInfo(micropixel_graphics_info_t& i) {
    i = {};
    i.size = sizeof(i);
    i.width = LCD_W;
    i.height = LCD_H;
    i.pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565;
    i.native_pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565;
    i.max_full_frame_fps = 60;
    return ready_ ? ok : MICROPIXEL_STATUS_INTERNAL;
}
void Graphics::ReleaseReferences() {
    for (size_t i = 0; i < texture_count_; ++i)
        if (retained_.release) retained_.release(retained_.context, textures_[i]);
    for (size_t i = 0; i < font_count_; ++i) fonts_.ReleaseSceneFont(font_handles_[i]);
    texture_count_ = font_count_ = 0;
    retained_ = {};
}
int32_t Graphics::Submit(const uint8_t* bytes, uint32_t n, const d::TextureAccess& t) {
    std::lock_guard lock(mutex_);
    if (direct_.buffer_count) return MICROPIXEL_STATUS_STALE_STATE;
    auto cap = g::ReadSceneCapacity(bytes, n);
    if (!cap) return cap.error();
    if (!t.resolve || !storage_.Ensure(*cap, scene_ ? &*scene_ : nullptr, compositor_ ? &*compositor_ : nullptr))
        return oom;
    if (!scene_) scene_.emplace(storage_.SceneView());
    if (!compositor_) compositor_.emplace(storage_.SurfaceView(), pixels_, g::DamageMergePolicy{256, 48000}, &text_);
    int32_t result = scene_->Apply(
        bytes, n, LCD_W, LCD_H, t.resolve, t.context,
        [](void* c, micropixel_font_handle_t f) { return ((Graphics*)c)->fonts_.ResolveGuestHandle(f) != nullptr; },
        this);
    if (result != ok) return result;
    micropixel_texture_handle_t next[32]{};
    micropixel_font_handle_t nf[12]{};
    size_t nt = 0, nfc = 0;
    for (uint16_t j = 0; j < scene_->NodeCount(); ++j) {
        auto& node = scene_->Nodes()[j];
        if (node.kind == g::GuestSceneNodeKind::kTexture ||
            (node.kind == g::GuestSceneNodeKind::kSpriteBatch && node.texture_handle)) {
            bool exists = false;
            for (size_t k = 0; k < nt; ++k) exists |= next[k] == node.texture_handle;
            if (!exists) {
                if (nt == 32) return oom;
                next[nt++] = node.texture_handle;
            }
        } else if (node.kind == g::GuestSceneNodeKind::kText) {
            bool exists = false;
            for (size_t k = 0; k < nfc; ++k) exists |= nf[k] == node.font_handle;
            if (!exists) {
                if (nfc == 12) return oom;
                nf[nfc++] = node.font_handle;
            }
        }
    }
    size_t rt = 0, rf = 0;
    while (rf < nfc && fonts_.RetainSceneFont(nf[rf])) ++rf;
    if (rf == nfc)
        while (rt < nt && t.retain && t.retain(t.context, next[rt])) ++rt;
    if (rf != nfc || rt != nt) {
        for (size_t j = 0; j < rf; ++j) fonts_.ReleaseSceneFont(nf[j]);
        for (size_t j = 0; j < rt; ++j) t.release(t.context, next[j]);
        return oom;
    }
    auto dest = Acquire();
    auto composed = compositor_->PresentScene(*scene_, dest, t.resolve, t.context);
    if (composed.status != g::AppSurfaceStatus::kOk || (!suspended_ && !Present(dest))) {
        for (size_t j = 0; j < rf; ++j) fonts_.ReleaseSceneFont(nf[j]);
        for (size_t j = 0; j < rt; ++j) t.release(t.context, next[j]);
        compositor_->Reset();
        return MICROPIXEL_STATUS_INTERNAL;
    }
    ReleaseReferences();
    retained_ = t;
    texture_count_ = nt;
    font_count_ = nfc;
    memcpy(textures_, next, nt * sizeof(*next));
    memcpy(font_handles_, nf, nfc * sizeof(*nf));
    return ok;
}
int32_t Graphics::LoadFont(const d::FontResourceView& v, micropixel_font_info_t& i) {
    std::lock_guard l(mutex_);
    return fonts_.LoadFont({v.data, v.size}, i);
}
int32_t Graphics::ReleaseFont(micropixel_font_handle_t f) {
    std::lock_guard l(mutex_);
    return fonts_.ReleaseFont(f);
}
int32_t Graphics::MeasureText(micropixel_font_handle_t f, const char* t, uint32_t n, micropixel_text_metrics_t& i) {
    std::lock_guard l(mutex_);
    if (!t || !n || n > d::graphics_limits::kMaxTextBytes) return invalid;
    auto* font = fonts_.ResolveGuestHandle(f);
    if (!font) return invalid;
    char str[d::graphics_limits::kMaxTextBytes + 1];
    memcpy(str, t, n);
    str[n] = 0;
    lv_point_t size{};
    lv_text_get_size(&size, str, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    i = {};
    i.size = sizeof(i);
    i.width = size.x;
    i.height = size.y;
    i.baseline = font->line_height - font->base_line;
    return ok;
}
int32_t Graphics::DrawText(const d::TextTarget& dest, int32_t x, int32_t y, uint32_t color, micropixel_font_handle_t f,
                           const char* t, uint32_t n) {
    std::lock_guard l(mutex_);
    if (!t || n > d::graphics_limits::kMaxTextBytes) return invalid;
    auto* font = fonts_.ResolveGuestHandle(f);
    if (!font) return invalid;
    return text_.DrawWithFont(Target(dest), x, y, color, font, t, n) ? ok : invalid;
}
int32_t Graphics::ScaleBitmap(const d::BitmapView& s, const d::BitmapView& d) {
    std::lock_guard l(mutex_);
    if (s.pixel_format != d.pixel_format || d.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888)
        return MICROPIXEL_STATUS_UNSUPPORTED;
    auto v = Surface(d);
    g::PixelSurface dest{const_cast<uint8_t*>(v.pixels), v.size, v.width, v.height, v.stride, v.format};
    return pixels_.Blit(Surface(s), {0, 0, (int)s.width, (int)s.height}, dest, {0, 0, (int)d.width, (int)d.height}, 255)
               ? ok
               : invalid;
}
int32_t Graphics::ShowLaunchBitmap(const d::BitmapView& b) {
    std::lock_guard l(mutex_);
    auto dest = Acquire();
    if (!dest.pixels) return oom;
    bool good = pixels_.Fill(dest, {0, 0, LCD_W, LCD_H}, 0, 255) &&
                pixels_.Blit(Surface(b), {0, 0, (int)b.width, (int)b.height}, dest, {0, 0, LCD_W, LCD_H}, 255) &&
                Present(dest);
    return good ? ok : invalid;
}
void Graphics::ReleaseGuestResources() {
    std::lock_guard l(mutex_);
    Drain();
    direct_ = {};
    sink_ = {};
    suspended_ = false;
    ReleaseReferences();
    compositor_.reset();
    scene_.reset();
    storage_.Reset();
    fonts_.ReleaseGuestFonts();
}
int32_t Graphics::CreateDirectSurface(const d::DirectSurfaceConfig& c, const d::DirectSurfaceReleaseSink& s,
                                      d::DirectSurfaceInfo& i) {
    std::lock_guard l(mutex_);
    if (direct_.buffer_count) return oom;
    if (c.width != LCD_W || c.height != LCD_H || c.pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB565 || !c.buffer_count ||
        c.buffer_count > 3 || c.flags || !s.release)
        return invalid;
    direct_ = c;
    sink_ = s;
    i = {MICROPIXEL_PIXEL_FORMAT_RGB565, 0, 60};
    return ok;
}
int32_t Graphics::PresentDirectSurface(const d::DirectSurfacePresentation& p) {
    std::lock_guard l(mutex_);
    if (!direct_.buffer_count) return MICROPIXEL_STATUS_STALE_STATE;
    if (!p.pixels || p.buffer_index >= direct_.buffer_count || !p.source_width || !p.source_height ||
        p.pitch < (uint64_t)p.source_width * 2 || (uint64_t)p.pitch * p.source_height > p.length)
        return invalid;
    if (!suspended_) {
        auto dest = Acquire();
        if (!dest.pixels) return MICROPIXEL_STATUS_INTERNAL;
        if (!p.byte_swapped && p.source_width == dest.width && p.source_height == dest.height) {
            // Native opaque RGB565 needs no color conversion. Prefer DMA2D;
            // unaligned sources can still use the CPU copy without repacking.
            const size_t row_bytes = static_cast<size_t>(dest.width) * 2U;
            if (media_display_copy_direct_rgb565(p.pixels, p.pitch, reinterpret_cast<uint16_t*>(dest.pixels))) {
                // The host overlay is drawn after DMA completes, before scanout.
            } else if (p.pitch == row_bytes && dest.stride == row_bytes) {
                memcpy(dest.pixels, p.pixels, row_bytes * dest.height);
            } else {
                for (uint32_t y = 0; y < dest.height; ++y) {
                    memcpy(dest.pixels + y * dest.stride, p.pixels + y * p.pitch, row_bytes);
                }
            }
        } else {
            g::ConstPixelSurface src{p.pixels,        p.length, p.source_width,
                                     p.source_height, p.pitch,  Format(MICROPIXEL_PIXEL_FORMAT_RGB565, p.byte_swapped)};
            if (!pixels_.Blit(src, {0, 0, (int)p.source_width, (int)p.source_height}, dest, {0, 0, LCD_W, LCD_H}, 255))
                return MICROPIXEL_STATUS_INTERNAL;
        }
        if (!Present(dest)) return MICROPIXEL_STATUS_INTERNAL;
    }
    ReleaseJob job{sink_, p.buffer_index, nullptr};
    return xQueueSend(releases_, &job, portMAX_DELAY) == pdTRUE ? ok : oom;
}
void Graphics::SuspendDirectSurface() {
    std::lock_guard l(mutex_);
    suspended_ = true;
    Drain();
}
void Graphics::ResumeDirectSurface() {
    std::lock_guard l(mutex_);
    suspended_ = false;
}
int32_t Graphics::DestroyDirectSurface() {
    std::lock_guard l(mutex_);
    Drain();
    direct_ = {};
    sink_ = {};
    return ok;
}
void Graphics::ShowMessage(const char* t, uint32_t color) {
    std::lock_guard l(mutex_);
    auto dest = Acquire();
    if (!dest.pixels) return;
    (void)pixels_.Fill(dest, {0, 0, LCD_W, LCD_H}, color, 255);
    (void)text_.Draw(dest, 30, 30, 0xffffff, MICROPIXEL_SYSTEM_FONT_TITLE, t, strlen(t));
    (void)Present(dest);
}
#ifdef MPX_PANEL_DIAGNOSTICS
void Graphics::SetDisplayByteSwap(bool enabled) {
    std::lock_guard lock(mutex_);
    media_display_set_rgb565_byte_swap(enabled);
    auto dest = Acquire();
    if (dest.pixels)
        (void)media_display_present_direct_rgb565(reinterpret_cast<uint16_t*>(dest.pixels), 0, 0, LCD_W, LCD_H);
}
void Graphics::SetDisplayPanelColors(bool bgr, bool inverted) {
    std::lock_guard lock(mutex_);
    media_display_set_panel_colors(bgr, inverted);
}
#endif
bool Graphics::ShowColorTest() {
    std::lock_guard lock(mutex_);
    auto dest = Acquire();
    if (!dest.pixels) return false;
    static constexpr uint32_t colors[] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff,
                                          0x808080, 0x000000, 0xeeeade, 0x182838};
    static constexpr const char* labels[] = {"RED", "GREEN", "BLUE", "WHITE", "GRAY", "BLACK", "CREAM", "NAVY"};
    for (unsigned i = 0; i < 8; ++i) {
        const int y = i * LCD_H / 8, h = (i + 1) * LCD_H / 8 - y;
        if (!pixels_.Fill(dest, {0, y, LCD_W, h}, colors[i], 255)) return false;
        const uint32_t ink = (i == 1 || i == 3 || i == 6) ? 0 : 0xffffff;
        if (!text_.Draw(dest, 20, y + 16, ink, MICROPIXEL_SYSTEM_FONT_TITLE, labels[i], strlen(labels[i])))
            return false;
    }
    return media_display_present_direct_rgb565(reinterpret_cast<uint16_t*>(dest.pixels), 0, 0, LCD_W, LCD_H);
}

}  // namespace mpx_port
