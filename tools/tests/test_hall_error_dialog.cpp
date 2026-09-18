// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "host/ui/lvgl/square_common/hall_error_dialog.hpp"

namespace {
using micropixel::host_ui::lvgl::square_common::DrawHallErrorDialog;
std::vector<uint32_t> pixels;
int display_width{};
unsigned dismissals{};
void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* data) {
    const auto* source = reinterpret_cast<const uint32_t*>(data);
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) pixels[y * display_width + x] = *source++;
    }
    lv_display_flush_ready(display);
}
void Close(lv_event_t*) { ++dismissals; }
void Capture(lv_display_t* display, int width, int height) {
    lv_refr_now(display);
    char name[64];
    std::snprintf(name, sizeof(name), "hall-error-%dx%d.ppm", width, height);
    FILE* file = std::fopen(name, "wb");
    Check(file != nullptr, "open screenshot");
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (auto pixel : pixels) {
        const std::array<unsigned char, 3> rgb{static_cast<unsigned char>(pixel >> 16U),
                                               static_cast<unsigned char>(pixel >> 8U),
                                               static_cast<unsigned char>(pixel)};
        std::fwrite(rgb.data(), 1, rgb.size(), file);
    }
    std::fclose(file);
}
void TestSize(int width, int height) {
    display_width = width;
    pixels.assign(width * height, 0);
    std::vector<uint32_t> buffer(width * height);
    lv_display_t* display = lv_display_create(width, height);
    lv_display_set_flush_cb(display, Flush);
    lv_display_set_buffers(display, buffer.data(), nullptr, buffer.size() * sizeof(uint32_t),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_obj_t* root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(0x08111f), 0);
    lv_obj_t* background = lv_label_create(root);
    lv_label_set_text(background, "App Hall\n\n[ App 1 ]    [ App 2 ]    [ App 3 ]");
    lv_obj_set_pos(background, 12, 20);
    const auto dialog = DrawHallErrorDialog(
        root, width, height,
        {.title = "App failed",
         .app_id = "micropixel.chinese-demo",
         .identity = "Required font: Chinese (Simplified)",
         .detail = "In Settings > Language, switch to Chinese (Simplified), then open this app again.",
         .close = "Close"},
        width <= 320 ? &lv_font_montserrat_14 : &lv_font_montserrat_24,
        width <= 320 ? &lv_font_montserrat_10 : &lv_font_montserrat_14, Close, nullptr);
    lv_obj_update_layout(root);
    lv_area_t button{}, body{}, panel{};
    lv_obj_get_coords(dialog.close, &button);
    lv_obj_get_coords(dialog.body, &body);
    lv_obj_get_coords(dialog.panel, &panel);
    Check(panel.x1 >= 0 && panel.y1 >= 0 && panel.x2 < width && panel.y2 < height, "dialog stays inside screen");
    Check(button.y1 > body.y2 && button.y2 < height && button.x2 < width, "close is pinned outside scrolling text");
    Check(body.y2 > body.y1, "text gets a visible viewport");
    Check(lv_obj_is_clickable(dialog.overlay), "scrim intercepts background taps");
    Check(lv_obj_get_child(root, -1) == dialog.overlay, "dialog covers the Hall");
    Check(lv_obj_get_scroll_bottom(dialog.body) <= 0, "font instruction is fully visible at each target size");
    Capture(display, width, height);

    lv_obj_t* detail = lv_obj_get_child(dialog.body, -1);
    lv_label_set_text(detail,
                      "A very long diagnostic must remain readable.\nLine 2\nLine 3\nLine 4\nLine 5\nLine 6\nLine 7\n"
                      "Line 8\nLine 9\nLine 10\nLine 11\nLine 12\nLine 13\nLine 14\nLine 15\nEnd of diagnostic.");
    lv_obj_update_layout(root);
    Check(lv_obj_get_scroll_bottom(dialog.body) > 0, "long diagnostics scroll");
    lv_obj_scroll_to_y(dialog.body, LV_COORD_MAX, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    lv_area_t after{};
    lv_obj_get_coords(dialog.close, &after);
    Check(button.x1 == after.x1 && button.y1 == after.y1 && button.y2 == after.y2,
          "scrolling cannot displace the close button");
    const unsigned before = dismissals;
    lv_obj_send_event(dialog.close, LV_EVENT_SHORT_CLICKED, nullptr);
    Check(dismissals == before + 1, "close emits dismissal");
    lv_display_delete(display);
}
}  // namespace
int main() {
    lv_init();
    TestSize(320, 240);
    TestSize(480, 480);
    TestSize(720, 720);
    std::puts("Hall error dialog LVGL tests passed (320x240, 480x480, 720x720).");
    lv_deinit();
}
