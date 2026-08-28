#include "platform/lvgl/ui/square_common/hall_scene_ui.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

#include "platform/lvgl/ui/square_common/icons/wifi_status_icons.hpp"

namespace micropixel::platform::lvgl::square_common {
namespace {

constexpr uint32_t kBackgroundRgb = 0x08111fU;

void StyleContainer(lv_obj_t* object, const HallSceneRect& bounds, int32_t radius, uint32_t color) {
    lv_obj_set_pos(object, bounds.x, bounds.y);
    lv_obj_set_size(object, bounds.width, bounds.height);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t* CreateLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color, HallScenePoint point) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, point.x, point.y);
    return label;
}

const char* HallStatusText(host_ui::HallStatus status) {
    switch (status) {
        case host_ui::HallStatus::kReady:
            return "Choose an app";
        case host_ui::HallStatus::kNoApps:
            return "No installed apps";
        case host_ui::HallStatus::kAppExited:
            return "App closed - choose another";
        case host_ui::HallStatus::kAppFailed:
            return "App failed";
        case host_ui::HallStatus::kRuntimeUnavailable:
            return "Runtime unavailable";
        case host_ui::HallStatus::kHostFailure:
            return "Host could not start the app";
    }
    return "Unavailable";
}

uint32_t HallStatusColor(host_ui::HallStatus status) {
    return status == host_ui::HallStatus::kReady || status == host_ui::HallStatus::kAppExited ? 0x4dd6a4U : 0xff8b73U;
}

const lv_image_dsc_t* HallWifiImage(const host_ui::HallWifiModel& model) {
    if (!model.available || !model.enabled || !model.connected) {
        return nullptr;
    }
    if (model.rssi < -75) {
        return &micropixel_wifi_status_weak;
    }
    if (model.rssi < -60) {
        return &micropixel_wifi_status_medium;
    }
    return &micropixel_wifi_status_full;
}

const char* HallBatterySymbol(const host_ui::HallBatteryModel& model) {
    if (model.charging) {
        return LV_SYMBOL_CHARGE;
    }
    if (model.percent <= 10U) {
        return LV_SYMBOL_BATTERY_EMPTY;
    }
    if (model.percent <= 35U) {
        return LV_SYMBOL_BATTERY_1;
    }
    if (model.percent <= 60U) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (model.percent <= 85U) {
        return LV_SYMBOL_BATTERY_3;
    }
    return LV_SYMBOL_BATTERY_FULL;
}

uint32_t PressedBackground(uint32_t color) {
    const uint32_t red = ((color >> 16U) & 0xffU) * 3U / 5U;
    const uint32_t green = ((color >> 8U) & 0xffU) * 3U / 5U;
    const uint32_t blue = (color & 0xffU) * 3U / 5U;
    return (red << 16U) | (green << 8U) | blue;
}

lv_obj_t* CreateHeaderButton(lv_obj_t* root, const HallSceneRect& bounds, int32_t radius, uint32_t border_color,
                             uint32_t background, uint32_t text_color, const char* icon, const char* text,
                             const HallSceneLayout& layout, bool center_content, lv_event_cb_t event, void* context,
                             bool enabled = true) {
    lv_obj_t* button = lv_button_create(root);
    StyleContainer(button, bounds, radius, background);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(border_color), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(PressedBackground(background)), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(border_color), LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, event, LV_EVENT_SHORT_CLICKED, context);
    if (!enabled) {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    if (center_content) {
        lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(button, layout.header_button_pad_horizontal, 0);
        lv_obj_set_style_pad_right(button, layout.header_button_pad_horizontal, 0);
        lv_obj_set_style_pad_column(button, 8, 0);
    }

    lv_obj_t* icon_label = lv_label_create(button);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, BuiltinLatinFont(layout.header_button_font), 0);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(text_color), 0);
    if (!center_content) {
        lv_obj_align(icon_label, LV_ALIGN_LEFT_MID, layout.header_button_icon_x, 0);
    }

    lv_obj_t* text_label = lv_label_create(button);
    lv_label_set_text(text_label, text);
    lv_obj_set_style_text_font(text_label, BuiltinLatinFont(layout.header_button_font), 0);
    lv_obj_set_style_text_color(text_label, lv_color_hex(text_color), 0);
    if (!center_content) {
        lv_obj_align(text_label, LV_ALIGN_LEFT_MID, layout.header_button_label_x, 0);
    }

    return button;
}

}  // namespace

bool HallStatusBarMatches(const host_ui::HallStatusBarModel& left, const host_ui::HallStatusBarModel& right) {
    return left.time_text == right.time_text && left.wifi.ssid == right.wifi.ssid &&
           left.wifi.rssi == right.wifi.rssi && left.wifi.available == right.wifi.available &&
           left.wifi.enabled == right.wifi.enabled && left.wifi.connected == right.wifi.connected &&
           left.cellular.signal_bars == right.cellular.signal_bars &&
           left.cellular.available == right.cellular.available && left.cellular.enabled == right.cellular.enabled &&
           left.cellular.connected == right.cellular.connected && left.battery.percent == right.battery.percent &&
           left.battery.available == right.battery.available && left.battery.charging == right.battery.charging;
}

void HallSceneUi::ResetLocked() {
    layout_ = nullptr;
    events_ = {};
    objects_ = {};
    settings_button_ = nullptr;
    update_button_ = nullptr;
}

void HallSceneUi::DrawLocked(lv_obj_t* root, const HallSceneLayout& layout, const host_ui::HallModel& model,
                             uint32_t visible_count, HallSceneEvents events) {
    ResetLocked();
    if (root == nullptr) {
        return;
    }
    layout_ = &layout;
    events_ = events;

    StyleContainer(root, {.x = 0, .y = 0, .width = layout.width, .height = layout.height}, 0, kBackgroundRgb);

    lv_obj_t* brand = lv_obj_create(root);
    StyleContainer(brand, layout.brand, layout.brand_radius, 0xff9f43U);
    lv_obj_remove_flag(brand, LV_OBJ_FLAG_CLICKABLE);

    (void)CreateLabel(root, "App Hall", BuiltinLatinFont(SystemFontRole::kTitle), 0xf2f7ffU, layout.title);
    char app_count_text[24]{};
    (void)std::snprintf(app_count_text, sizeof(app_count_text), "INSTALLED APPS (%" PRIu32 ")", visible_count);
    (void)CreateLabel(root, app_count_text, BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, layout.section);

    settings_button_ =
        CreateHeaderButton(root, layout.settings_button, layout.header_button_radius, 0x42607fU, 0x111f32U, 0xf2f7ffU,
                           LV_SYMBOL_SETTINGS, "Settings", layout, true, HeaderButtonEvent, this);
    if (model.firmware_update_available) {
        update_button_ =
            CreateHeaderButton(root, layout.update_button, layout.header_button_radius, 0x5e9f59U, 0x152a2aU, 0xcdf5c6U,
                               LV_SYMBOL_REFRESH, "Update", layout, false, HeaderButtonEvent, this);
        lv_obj_t* update_dot = lv_obj_create(update_button_);
        StyleContainer(update_dot, {.x = layout.update_button.width - 19, .y = 8, .width = 10, .height = 10},
                       LV_RADIUS_CIRCLE, 0xef5d4fU);
        lv_obj_remove_flag(update_dot, LV_OBJ_FLAG_CLICKABLE);
    }

    objects_.carousel_viewport = lv_obj_create(root);
    StyleContainer(objects_.carousel_viewport, layout.carousel, 0, kBackgroundRgb);
    lv_obj_set_scroll_dir(objects_.carousel_viewport, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(objects_.carousel_viewport, LV_SCROLLBAR_MODE_OFF);
    constexpr lv_obj_flag_t kScrollFlags = static_cast<lv_obj_flag_t>(
        static_cast<uint32_t>(LV_OBJ_FLAG_SCROLLABLE) | static_cast<uint32_t>(LV_OBJ_FLAG_SCROLL_MOMENTUM) |
        static_cast<uint32_t>(LV_OBJ_FLAG_SCROLL_ELASTIC));
    lv_obj_add_flag(objects_.carousel_viewport, kScrollFlags);
    lv_obj_add_flag(objects_.carousel_viewport, LV_OBJ_FLAG_CLICKABLE);
    if (events.carousel_event != nullptr) {
        lv_obj_add_event_cb(objects_.carousel_viewport, events.carousel_event, LV_EVENT_SCROLL,
                            events.carousel_context);
        lv_obj_add_event_cb(objects_.carousel_viewport, events.carousel_event, LV_EVENT_SCROLL_END,
                            events.carousel_context);
    }

    objects_.carousel_content = lv_obj_create(objects_.carousel_viewport);
    const int32_t cards_width = visible_count == 0U
                                    ? layout.carousel.width
                                    : static_cast<int32_t>(visible_count) * (layout.card_width + layout.card_gap) -
                                          layout.card_gap + layout.content_trailing_width;
    StyleContainer(objects_.carousel_content, {.x = 0, .y = 0, .width = cards_width, .height = layout.carousel.height},
                   0, kBackgroundRgb);
    lv_obj_set_style_bg_opa(objects_.carousel_content, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(objects_.carousel_content, LV_OBJ_FLAG_CLICKABLE);

    if (visible_count > layout.fully_visible_cards) {
        objects_.scroll_track = lv_obj_create(root);
        StyleContainer(objects_.scroll_track, layout.scroll_track, layout.scroll_track.height / 2, 0x21364eU);
        lv_obj_remove_flag(objects_.scroll_track, LV_OBJ_FLAG_CLICKABLE);
        objects_.scroll_thumb = lv_obj_create(objects_.scroll_track);
        const int32_t thumb_width =
            std::max<int32_t>(layout.scroll_min_thumb_width, layout.scroll_track.width *
                                                                 static_cast<int32_t>(layout.fully_visible_cards) /
                                                                 static_cast<int32_t>(visible_count));
        StyleContainer(objects_.scroll_thumb,
                       {.x = 0, .y = 0, .width = thumb_width, .height = layout.scroll_track.height},
                       layout.scroll_track.height / 2, 0x69a7ffU);
        lv_obj_remove_flag(objects_.scroll_thumb, LV_OBJ_FLAG_CLICKABLE);
    }

    if (visible_count == 0U) {
        (void)CreateLabel(root, "No readable Bundle in App Store", BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU,
                          layout.empty_message);
    }

    if (model.status == host_ui::HallStatus::kAppFailed) {
        (void)CreateLabel(root, HallStatusText(model.status), BuiltinLatinFont(SystemFontRole::kMedium),
                          HallStatusColor(model.status), layout.failure_status);
        if (model.status_app_id != nullptr && model.status_app_id[0] != '\0') {
            lv_obj_t* label = CreateLabel(root, model.status_app_id, BuiltinLatinFont(SystemFontRole::kSmall),
                                          0x91a4bdU, layout.failure_app_id);
            lv_obj_set_width(label, layout.status_text_width);
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        }
        char identity[128]{};
        const char* phase = model.status_error_phase != nullptr ? model.status_error_phase : "unknown";
        const char* code = model.status_error_code != nullptr ? model.status_error_code : "app_failed";
        if (model.status_has_exit_code) {
            (void)std::snprintf(identity, sizeof(identity), "%s / %s / exit=%" PRId32, phase, code,
                                model.status_exit_code);
        } else {
            (void)std::snprintf(identity, sizeof(identity), "%s / %s", phase, code);
        }
        lv_obj_t* identity_label =
            CreateLabel(root, identity, BuiltinLatinFont(SystemFontRole::kSmall), 0xffb29fU, layout.failure_identity);
        lv_obj_set_width(identity_label, layout.status_text_width);
        lv_label_set_long_mode(identity_label, LV_LABEL_LONG_DOT);
        if (model.status_error_detail != nullptr && model.status_error_detail[0] != '\0') {
            lv_obj_t* detail = CreateLabel(root, model.status_error_detail, BuiltinLatinFont(SystemFontRole::kSmall),
                                           0xc8d5e5U, layout.failure_detail);
            lv_obj_set_width(detail, layout.status_text_width);
            lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
        }
    } else {
        if (model.status != host_ui::HallStatus::kReady) {
            (void)CreateLabel(root, HallStatusText(model.status), BuiltinLatinFont(SystemFontRole::kMedium),
                              HallStatusColor(model.status), layout.simple_status);
        }
        if (model.status_app_id != nullptr && model.status_app_id[0] != '\0') {
            lv_obj_t* label = CreateLabel(root, model.status_app_id, BuiltinLatinFont(SystemFontRole::kMedium),
                                          0x91a4bdU, layout.simple_app_id);
            lv_obj_set_width(label, layout.status_text_width);
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        }
    }

    const HallStatusBarLayout& status = layout.status_bar;
    objects_.time_label = CreateLabel(root, model.status_bar.time_text.data(),
                                      BuiltinLatinFont(SystemFontRole::kMedium), 0xf2f7ffU, status.time);
    lv_obj_set_width(objects_.time_label, status.time_width);
    objects_.cellular_container = lv_obj_create(root);
    StyleContainer(objects_.cellular_container, status.cellular, 0, kBackgroundRgb);
    lv_obj_set_style_bg_opa(objects_.cellular_container, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(objects_.cellular_container, LV_OBJ_FLAG_CLICKABLE);
    for (uint32_t index = 0U; index < objects_.cellular_bars.size(); ++index) {
        lv_obj_t* bar = lv_obj_create(objects_.cellular_container);
        const int32_t height = std::max<int32_t>(2, status.cellular.height * static_cast<int32_t>(index + 1U) / 5);
        StyleContainer(bar,
                       {.x = static_cast<int32_t>(index) * status.cellular.width / 5,
                        .y = status.cellular.height - height,
                        .width = std::max<int32_t>(2, status.cellular.width / 7),
                        .height = height},
                       1, 0xf2f7ffU);
        objects_.cellular_bars[index] = bar;
    }
    objects_.wifi_image = lv_image_create(root);
    lv_obj_set_pos(objects_.wifi_image, status.wifi.x, status.wifi.y);
    lv_image_set_scale(objects_.wifi_image, status.wifi_scale);
    lv_obj_set_style_image_recolor(objects_.wifi_image, lv_color_hex(0xf2f7ffU), 0);
    lv_obj_set_style_image_recolor_opa(objects_.wifi_image, LV_OPA_COVER, 0);
    objects_.battery_label = CreateLabel(root, "", BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, status.battery);
    lv_obj_set_width(objects_.battery_label, status.battery_width);
    lv_obj_set_style_text_align(objects_.battery_label, LV_TEXT_ALIGN_CENTER, 0);
    objects_.battery_percent_label =
        CreateLabel(root, "", BuiltinLatinFont(SystemFontRole::kSmall), 0xf2f7ffU, status.battery_percent);
    lv_obj_set_width(objects_.battery_percent_label, status.battery_percent_width);
    lv_obj_set_style_text_align(objects_.battery_percent_label, LV_TEXT_ALIGN_RIGHT, 0);
    UpdateStatusBarLocked(model.status_bar);
}

void HallSceneUi::UpdateStatusBarLocked(const host_ui::HallStatusBarModel& model) {
    if (objects_.time_label == nullptr || objects_.cellular_container == nullptr || objects_.wifi_image == nullptr ||
        objects_.battery_label == nullptr || objects_.battery_percent_label == nullptr) {
        return;
    }
    lv_label_set_text(objects_.time_label, model.time_text.data());
    if (model.cellular.available) {
        const uint32_t bars = model.cellular.enabled && model.cellular.connected
                                  ? std::min<uint32_t>(model.cellular.signal_bars, objects_.cellular_bars.size())
                                  : 0U;
        for (uint32_t index = 0U; index < objects_.cellular_bars.size(); ++index) {
            lv_obj_set_style_bg_opa(objects_.cellular_bars[index], index < bars ? LV_OPA_COVER : 72, 0);
        }
        lv_obj_remove_flag(objects_.cellular_container, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(objects_.cellular_container, LV_OBJ_FLAG_HIDDEN);
    }
    if (const lv_image_dsc_t* wifi = HallWifiImage(model.wifi); wifi != nullptr) {
        lv_image_set_src(objects_.wifi_image, wifi);
        lv_obj_remove_flag(objects_.wifi_image, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(objects_.wifi_image, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(objects_.battery_label, model.battery.available || model.battery.charging
                                                  ? HallBatterySymbol(model.battery)
                                                  : LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_set_y(objects_.battery_label, layout_->status_bar.battery.y + (model.battery.charging ? 3 : 0));
    lv_obj_set_style_text_font(
        objects_.battery_label,
        model.battery.charging ? BuiltinLatinFont(SystemFontRole::kMedium) : BuiltinLatinFont(SystemFontRole::kLarge),
        0);
    lv_obj_set_style_transform_scale_x(objects_.battery_label, model.battery.charging ? 256 : 282, 0);
    char percent[8]{};
    if (model.battery.available) {
        (void)std::snprintf(percent, sizeof(percent), "%u%%", static_cast<unsigned>(model.battery.percent));
    } else {
        (void)std::snprintf(percent, sizeof(percent), "--");
    }
    lv_label_set_text(objects_.battery_percent_label, percent);
    const uint32_t color = model.battery.charging ? 0x4dd6a4U : 0xf2f7ffU;
    lv_obj_set_style_text_color(objects_.battery_label, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(objects_.battery_percent_label, lv_color_hex(color), 0);
}

void HallSceneUi::UpdateScrollLocked(uint32_t app_count, int32_t offset) {
    if (layout_ == nullptr || objects_.scroll_thumb == nullptr || app_count <= layout_->fully_visible_cards) {
        return;
    }
    const int32_t step = layout_->card_width + layout_->card_gap;
    const int32_t maximum = static_cast<int32_t>(app_count - layout_->fully_visible_cards) * step;
    const int32_t thumb_width = lv_obj_get_width(objects_.scroll_thumb);
    const int32_t clamped = std::clamp<int32_t>(offset, 0, maximum);
    lv_obj_set_x(objects_.scroll_thumb,
                 maximum == 0 ? 0 : (layout_->scroll_track.width - thumb_width) * clamped / maximum);
}

void HallSceneUi::HeaderButtonEvent(lv_event_t* event) {
    auto* scene = static_cast<HallSceneUi*>(lv_event_get_user_data(event));
    if (scene != nullptr) {
        scene->HandleHeaderButtonEvent(event);
    }
}

void HallSceneUi::HandleHeaderButtonEvent(lv_event_t* event) {
    lv_obj_t* button = lv_event_get_current_target_obj(event);
    const bool update = button == update_button_;
    if (button == nullptr || (button != settings_button_ && button != update_button_)) {
        return;
    }
    if (lv_event_get_code(event) == LV_EVENT_SHORT_CLICKED && events_.action_sink != nullptr) {
        events_.action_sink(events_.action_context,
                            host_ui::SystemUiAction{.type = update ? host_ui::SystemUiActionType::kOpenFirmwareUpdate
                                                                   : host_ui::SystemUiActionType::kOpenSystemMenu});
    }
}

}  // namespace micropixel::platform::lvgl::square_common
