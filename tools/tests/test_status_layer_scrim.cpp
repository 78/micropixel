// SPDX-License-Identifier: Apache-2.0
//
// Portrait status-layer regression: the 720x1280 profile used to inherit the
// square 720x720 layout, so the quick-settings scrim left the bottom 560 px of
// the panel neither dimmed nor able to dismiss the sheet.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "host/ui/lvgl/square_common/status_layer_ui.hpp"

namespace micropixel::host_ui {
const char* DisplayLocale() { return "en"; }
}  // namespace micropixel::host_ui

namespace micropixel::platform::lvgl {
const lv_font_t* BuiltinLatinFont(SystemFontRole) { return &lv_font_montserrat_14; }
}  // namespace micropixel::platform::lvgl

namespace {

using namespace micropixel::host_ui;
using namespace micropixel::host_ui::lvgl::square_common;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void Action(void*, const SystemUiAction&) {}

// The scrim is the largest object the sheet builds on the screen, so the tallest
// and widest visible child is the measurable stand-in for "the scrim covers the
// display". Child order is an LVGL internal and is deliberately not assumed.
void LargestVisibleSize(lv_obj_t* parent, int32_t& width, int32_t& height) {
    const uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t index = 0; index < count; ++index) {
        lv_obj_t* child = lv_obj_get_child(parent, index);
        if (lv_obj_is_hidden(child)) {
            continue;
        }
        if (lv_obj_get_width(child) > width) {
            width = lv_obj_get_width(child);
        }
        if (lv_obj_get_height(child) > height) {
            height = lv_obj_get_height(child);
        }
        LargestVisibleSize(child, width, height);
    }
}

// StatusLayerUi creates the scrim as the first child of the active screen and
// sizes it from its layout, so that object is the observable the display height
// has to reach.
void CheckScrim(int32_t width, int32_t height, StatusLayerLayoutProfile profile, int32_t expected_width,
                int32_t expected_height, const char* message) {
    lv_display_t* display = lv_display_create(width, height);
    Check(display != nullptr, "create the test display");
    std::vector<uint32_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    lv_display_set_buffers(display, pixels.data(), nullptr, pixels.size() * 4U, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, [](lv_display_t* target, const lv_area_t*, uint8_t*) {
        lv_display_flush_ready(target);
    });

    SystemPageLayout layout{};
    layout.width = width;
    layout.height = height;
    StatusLayerUi ui(layout, profile);
    const StatusLayerModel model{};
    Check(ui.ShowLocked(model, Action, nullptr).has_value(), "show the status layer");
    Check(ui.VisibleLocked(), "the sheet is on screen");
    // LVGL computes layout and sizes on refresh, so geometry is only meaningful
    // after the timer handler has run.
    for (uint32_t frame = 0U; frame < 4U; ++frame) {
        lv_tick_inc(16U);
        (void)lv_timer_handler();
    }
    int32_t widest = 0;
    int32_t tallest = 0;
    LargestVisibleSize(lv_screen_active(), widest, tallest);
    if (widest != expected_width || tallest != expected_height) {
        std::fprintf(stderr, "FAIL: %s (largest object is %dx%d, expected %dx%d)\n", message, static_cast<int>(widest),
                     static_cast<int>(tallest), static_cast<int>(expected_width), static_cast<int>(expected_height));
        std::exit(1);
    }
}

}  // namespace

int main() {
    // LVGL's list pools only exist after lv_init(); without it lv_display_create
    // reads a zero node size and overruns its own allocation.
    lv_init();
    // Tab5: 720x1280 portrait, the panel the reviewed revision left uncovered.
    // The square and round panels keep kAutomatic and therefore their existing
    // layouts, which this fix does not touch.
    CheckScrim(720, 1280, StatusLayerLayoutProfile::kPortrait720x1280, 720, 1280,
               "the portrait scrim covers the whole display");
    lv_deinit();
    std::puts("status layer scrim tests passed");
    return 0;
}
