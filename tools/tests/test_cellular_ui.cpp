// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
unsigned actions{};
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
    lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    StatusLayerUi ui(layout);
    StatusLayerModel model{};
    model.cellular_available = model.cellular_enabled = model.open_cellular_settings = true;
    model.cellular_state = micropixel::device::CellularState::kConnecting;
    model.cellular_sim_pending = false;
    Check(ui.ShowLocked(model, Action, nullptr).has_value(), "show searching cellular page");
    auto* page = ui.TransitionDialogLocked();
    auto* control = FindSwitch(page);
    Check(control && lv_obj_has_state(control, LV_STATE_CHECKED), "radio starts enabled");
    auto* sections = lv_obj_get_child(lv_obj_get_parent(lv_obj_get_parent(control)), 1);
    auto* sim_choices = lv_obj_get_child(lv_obj_get_child(sections, 0), 1);
    auto* sim_button = lv_obj_get_child(sim_choices, 0);
    Check(lv_obj_has_flag(sections, LV_OBJ_FLAG_HIDDEN), "startup hides unsampled SIM details");
    for (unsigned i = 0; i < 4; ++i) ui.UpdateLocked(model);
    Check(lv_obj_has_flag(sections, LV_OBJ_FLAG_HIDDEN), "startup events do not prematurely expand details");
    model.cellular_diagnostics.sampled = true;
    model.cellular_diagnostics.incomplete = true;
    model.cellular_diagnostics.sim_status = micropixel::device::CellularSimStatus::kAbsent;
    ui.UpdateLocked(model);
    Check(!lv_obj_has_flag(sections, LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_state(sim_button, LV_STATE_DISABLED),
          "first sample reveals usable SIM selection even with no SIM or incomplete diagnostics");
    model.cellular_diagnostics.sim_status = micropixel::device::CellularSimStatus::kPinRequired;
    ui.UpdateLocked(model);
    Check(!lv_obj_has_flag(sections, LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_state(sim_button, LV_STATE_DISABLED),
          "later diagnostic updates preserve layout and interactive SIM controls");
    model.cellular_sim_pending = true;
    ui.UpdateLocked(model);
    Check(!lv_obj_has_flag(sections, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_state(sim_button, LV_STATE_DISABLED),
          "actual SIM switching disables selection without collapsing details");
    model.cellular_sim_pending = false;
    model.cellular_diagnostics = {};  // The SIM change restarts only the modem.
    ui.UpdateLocked(model);
    Check(!lv_obj_has_flag(sections, LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_state(sim_button, LV_STATE_DISABLED),
          "modem restart preserves an already revealed layout");
    actions = 0;
    Click(control);
    Check(actions == 1 && last.type == SystemUiActionType::kSetCellularEnabled && last.value == 0,
          "searching radio accepts off immediately");
    Check(lv_obj_has_state(control, LV_STATE_DISABLED), "first click locks switch before Host handles request");
    Check(HasText(page, "Turning 4G off..."), "first click explains pending shutdown");
    Check(lv_obj_has_flag(lv_obj_get_child(lv_obj_get_parent(lv_obj_get_parent(control)), 1), LV_OBJ_FLAG_HIDDEN),
          "off request immediately hides network details");
    const auto off_request = last;
    ui.UpdateLocked(model);  // A battery/network event was already queued before the click.
    Check(!lv_obj_has_state(control, LV_STATE_CHECKED) && lv_obj_has_state(control, LV_STATE_DISABLED),
          "stale model cannot undo requested off position or unlock switch");
    for (unsigned i = 0; i < 10; ++i) Click(control);
    Check(actions == 1, "rapid repeated input cannot enqueue opposite operation");
    model.cellular_command_ack_us = off_request.timestamp_us;
    model.cellular_switching = true;
    ui.UpdateLocked(model);
    Click(control);
    Check(actions == 1 && lv_obj_has_state(control, LV_STATE_DISABLED), "acknowledgment alone does not unlock");
    model.cellular_switching = model.cellular_enabled = false;
    ui.UpdateLocked(model);
    Click(control);
    Check(actions == 1 && lv_obj_has_state(control, LV_STATE_DISABLED),
          "fast shutdown cannot turn a double-click into off then on");
    lv_tick_inc(501);
    lv_timer_handler();
    Check(!lv_obj_has_state(control, LV_STATE_DISABLED) && !lv_obj_has_state(control, LV_STATE_CHECKED),
          "completed shutdown releases switch in off position");
    Click(control);
    Check(actions == 2 && last.value == 1 && HasText(page, "Turning 4G on..."), "later deliberate enable accepted");
    Check(lv_obj_has_flag(sections, LV_OBJ_FLAG_HIDDEN), "enabling again waits for a fresh diagnostic sample");
    model.cellular_command_ack_us = last.timestamp_us;  // Rejected by an OTA hold or full worker queue.
    ui.UpdateLocked(model);
    lv_tick_inc(501);
    lv_timer_handler();
    Check(!lv_obj_has_state(control, LV_STATE_DISABLED) && !lv_obj_has_state(control, LV_STATE_CHECKED),
          "rejection restores actual state without leaving disabled control");
    Click(control);
    model.cellular_command_ack_us = last.timestamp_us;
    model.cellular_enabled = true;  // Worker completed before the first acknowledgment reached UI.
    ui.UpdateLocked(model);
    lv_tick_inc(501);
    lv_timer_handler();
    Check(!lv_obj_has_state(control, LV_STATE_DISABLED) && lv_obj_has_state(control, LV_STATE_CHECKED),
          "fast completion does not strand pending latch");
    Check(lv_obj_has_flag(sections, LV_OBJ_FLAG_HIDDEN), "radio enabled alone does not reveal details");
    model.cellular_diagnostics.sampled = true;
    ui.UpdateLocked(model);
    Check(!lv_obj_has_flag(sections, LV_OBJ_FLAG_HIDDEN), "new enable cycle reveals its first sample");
    ui.LeaveLocked();
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
    std::puts("Cellular UI tests passed");
}
