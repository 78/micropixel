// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "host/ui/lvgl/square_common/wifi_settings_ui.hpp"

namespace micropixel::host_ui {
const char* DisplayLocale() { return "en"; }
}  // namespace micropixel::host_ui
namespace micropixel::platform::lvgl {
const lv_font_t* BuiltinLatinFont(SystemFontRole) { return &lv_font_montserrat_14; }
}  // namespace micropixel::platform::lvgl
namespace micropixel::host_ui::lvgl::square_common {
class StatusLayerTransition {};
void ActionSheetPresenter::RequestLocked(const SystemPageLayout&, lv_obj_t*, SystemUiActionSink, void*) {}
}  // namespace micropixel::host_ui::lvgl::square_common
namespace {
using namespace micropixel::host_ui;
using namespace micropixel::host_ui::lvgl::square_common;
void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
unsigned actions{};
unsigned frames{};
SystemUiAction last{};
void Action(void*, const SystemUiAction& action) {
    ++actions;
    last = action;
}
lv_obj_t* FindSwitch(lv_obj_t* parent) {
    if (lv_obj_check_type(parent, &lv_switch_class)) return parent;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i)
        if (auto* found = FindSwitch(lv_obj_get_child(parent, i))) return found;
    return nullptr;
}
bool HasText(lv_obj_t* parent, const char* text) {
    if (lv_obj_check_type(parent, &lv_label_class) && !std::strcmp(lv_label_get_text(parent), text)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i)
        if (HasText(lv_obj_get_child(parent, i), text)) return true;
    return false;
}
void Click(lv_obj_t* control) {
    // LVGL's base-object click handler changes CHECKED and emits VALUE_CHANGED.
    if (!lv_obj_has_state(control, LV_STATE_DISABLED)) lv_obj_send_event(control, LV_EVENT_RELEASED, nullptr);
}
void Run(const SystemPageLayout& layout) {
    auto* display = lv_display_create(layout.width, layout.height);
    std::vector<uint32_t> pixels(layout.width * layout.height);
    lv_display_set_buffers(display, pixels.data(), nullptr, pixels.size() * 4, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t*, uint8_t*) {
        ++frames;
        lv_display_flush_ready(d);
    });
    lv_timer_set_period(lv_display_get_refr_timer(display), 1000);
    StatusLayerTransition transition;
    ActionSheetPresenter sheets(transition);
    WifiSettingsUi ui(sheets);
    WifiSettingsModel model{};
    model.available = model.enabled = true;
    model.connection_state = WifiConnectionState::kConnecting;
    auto* root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(root, layout.width, layout.height);
    Check(ui.ShowLocked(root, display, layout, model, Action, nullptr, nullptr, nullptr).has_value(), "show Wi-Fi");
    auto control = [&] { return FindSwitch(root); };
    lv_obj_set_style_anim_duration(control(), 300, LV_PART_MAIN);
    for (unsigned i = 0; i < 20; ++i) {
        lv_tick_inc(16);
        lv_timer_handler();
    }
    frames = 0;
    actions = 0;
    Click(control());
    for (unsigned i = 0; i < 16; ++i) {
        lv_tick_inc(16);
        lv_timer_handler();
    }
    Check(frames >= 3, "native switch animation reaches display while static refresh timer is slow");
    Check(actions == 1 && last.value == 0 && lv_obj_has_state(control(), LV_STATE_DISABLED),
          "Wi-Fi click disables immediately before Host response");
    const auto request = last;
    ui.Update(model, false);
    Check(!lv_obj_has_state(control(), LV_STATE_CHECKED) && lv_obj_has_state(control(), LV_STATE_DISABLED),
          "old model cannot undo Wi-Fi request");
    lv_tick_inc(501);
    lv_timer_handler();
    ui.Update(model, false);
    Check(lv_obj_has_state(control(), LV_STATE_DISABLED), "slow Host acknowledgment retains disabled switch");
    Check(HasText(root, "Applying network change..."), "pending Wi-Fi request has visible feedback");
    model.command_ack_us = request.timestamp_us;
    model.enabled = false;
    model.control_pending = true;
    ui.Update(model, false);
    Click(control());
    Check(actions == 1, "driver RPC pending ignores repeated click");
    model.control_pending = false;
    ui.Update(model, false);
    Check(!lv_obj_has_state(control(), LV_STATE_DISABLED) && !lv_obj_has_state(control(), LV_STATE_CHECKED),
          "finished shutdown restores interactive off switch");
    Click(control());
    model.command_ack_us = last.timestamp_us;  // Configuration hold/worker rejection.
    ui.Update(model, false);
    lv_tick_inc(501);
    lv_timer_handler();
    ui.Update(model, false);
    Check(!lv_obj_has_state(control(), LV_STATE_DISABLED) && !lv_obj_has_state(control(), LV_STATE_CHECKED),
          "rejected Wi-Fi request restores actual state");
    Click(control());
    ui.Leave();  // Cancels pending render and guard callbacks before deleting the page.
    lv_obj_delete(root);
    lv_tick_inc(501);
    lv_timer_handler();
    lv_display_delete(display);
}
}  // namespace
int main() {
    lv_init();
    for (int width : {320, 480, 720}) {
        SystemPageLayout page{.width = width,
                              .height = width == 320 ? 240 : width,
                              .header_height = 54,
                              .safe_horizontal = 12,
                              .header_padding_top = 10,
                              .header_gap = 10,
                              .back_button_size = 34,
                              .back_button_radius = 10,
                              .back_button_hit_padding = 5,
                              .content_padding_top = 5,
                              .content_padding_bottom = 10,
                              .section_gap = 8,
                              .panel_gap = 5,
                              .panel_padding = 10,
                              .panel_radius = 10,
                              .row_height = 38,
                              .control_height = 40,
                              .scrollbar_width = 3,
                              .scrollbar_radius = 2};
        Run(page);
    }
    lv_deinit();
    std::puts("Wi-Fi UI tests passed");
}
