#include "platform/lvgl/ui/square_common/hall_card_ui.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"

namespace micropixel::platform::lvgl::square_common {
namespace {

constexpr std::array<uint32_t, 3> kCoverColors{0xff9f43U, 0x4dd6a4U, 0x69a7ffU};

const char* AppShortName(const HallCardPresentation& app) {
    if (app.display_name != nullptr && app.display_name[0] != '\0') {
        return app.display_name;
    }
    if (app.app_id == nullptr || app.app_id[0] == '\0') {
        return "APP";
    }
    const char* separator = std::strrchr(app.app_id, '.');
    return separator != nullptr && separator[1] != '\0' ? separator + 1 : app.app_id;
}

void StyleContainer(lv_obj_t* object, int32_t x, int32_t y, int32_t width, int32_t height, int32_t radius,
                    uint32_t color) {
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

}  // namespace

void DrawHallCard(lv_obj_t* parent, const HallCardLayout& layout, const HallCardPresentation& app, uint32_t index,
                  lv_event_cb_t card_event, lv_event_cb_t stop_event, void* event_context, HallCardObjects& objects) {
    objects = {};
    if (parent == nullptr || layout.width <= 0 || layout.height <= 0) {
        return;
    }

    lv_obj_t* card = lv_obj_create(parent);
    objects.card = card;
    lv_obj_set_size(card, layout.width, layout.height);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_radius(card, layout.radius, 0);
    lv_obj_set_style_border_width(card, layout.border_width, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2e4562U), 0);
    lv_obj_set_style_border_post(card, true, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111f32U), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    if (card_event != nullptr) {
        lv_obj_add_event_cb(card, card_event, LV_EVENT_ALL, event_context);
    }

    lv_obj_t* cover = lv_obj_create(card);
    StyleContainer(cover, -layout.border_width, -layout.border_width, layout.width, layout.width, layout.radius,
                   0x182d48U);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_CLICKABLE);

    objects.cover_placeholder = lv_obj_create(cover);
    StyleContainer(objects.cover_placeholder, 0, 0, layout.width, layout.width, layout.radius,
                   kCoverColors[index % kCoverColors.size()]);
    lv_obj_remove_flag(objects.cover_placeholder, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* cover_label = lv_label_create(objects.cover_placeholder);
    const char* short_name = AppShortName(app);
    char initial[2]{short_name[0] != '\0' ? short_name[0] : 'A', '\0'};
    lv_label_set_text(cover_label, initial);
    lv_obj_set_style_text_font(cover_label, BuiltinLatinFont(SystemFontRole::kLarge), 0);
    lv_obj_set_style_text_color(cover_label, lv_color_hex(0x08111fU), 0);
    lv_obj_center(cover_label);

    objects.cover_image = lv_image_create(cover);
    lv_obj_center(objects.cover_image);
    lv_obj_add_flag(objects.cover_image, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* app_label = lv_label_create(card);
    lv_label_set_text(
        app_label, app.display_name != nullptr && app.display_name[0] != '\0' ? app.display_name : AppShortName(app));
    lv_obj_set_pos(app_label, layout.label_x, layout.label_y);
    lv_obj_set_size(app_label, layout.label_width, layout.label_height);
    lv_label_set_long_mode(app_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(app_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(app_label, lv_color_hex(0xf2f7ffU), 0);
    lv_obj_set_style_text_font(app_label, BuiltinLatinFont(layout.label_font), 0);

    if (app.running) {
        lv_obj_t* running = lv_obj_create(card);
        StyleContainer(running, layout.running_x, layout.running_y, layout.running_width, layout.running_height,
                       layout.running_height / 2, 0x08111fU);
        lv_obj_set_style_bg_opa(running, LV_OPA_90, 0);
        lv_obj_remove_flag(running, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* running_label = lv_label_create(running);
        lv_label_set_text(running_label, "RUNNING");
        lv_obj_set_style_text_font(running_label, BuiltinLatinFont(layout.badge_font), 0);
        lv_obj_set_style_text_color(running_label, lv_color_hex(0x4dd6a4U), 0);
        lv_obj_center(running_label);

        lv_obj_t* stop = lv_button_create(card);
        StyleContainer(stop, layout.stop_x, layout.stop_y, layout.stop_size, layout.stop_size, layout.stop_size / 2,
                       0x08111fU);
        lv_obj_set_style_bg_color(stop, lv_color_hex(0x182d48U), static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
        lv_obj_set_style_bg_opa(stop, LV_OPA_90, 0);
        lv_obj_set_style_shadow_width(stop, 0, 0);
        lv_obj_set_ext_click_area(stop, std::max<int32_t>(layout.stop_hit_padding, 0));
        if (stop_event != nullptr) {
            lv_obj_add_event_cb(stop, stop_event, LV_EVENT_SHORT_CLICKED, event_context);
        }
        lv_obj_t* stop_icon = lv_obj_create(stop);
        StyleContainer(stop_icon, 0, 0, layout.stop_icon_size, layout.stop_icon_size,
                       std::max<int32_t>(2, layout.stop_icon_size / 6), 0xff5c5cU);
        lv_obj_remove_flag(stop_icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(stop_icon);
    }

    objects.press_overlay = lv_obj_create(card);
    StyleContainer(objects.press_overlay, -layout.border_width, -layout.border_width, layout.width, layout.height,
                   layout.radius, 0x000000U);
    lv_obj_set_style_bg_opa(objects.press_overlay, LV_OPA_40, 0);
    lv_obj_remove_flag(objects.press_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(objects.press_overlay, LV_OBJ_FLAG_HIDDEN);
}

void SetHallCardPressed(const HallCardObjects& objects, bool pressed) {
    if (objects.press_overlay == nullptr) {
        return;
    }
    if (pressed) {
        lv_obj_remove_flag(objects.press_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(objects.press_overlay);
    } else {
        lv_obj_add_flag(objects.press_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void ShowHallCoverPlaceholder(const HallCardObjects& objects) {
    if (objects.cover_image != nullptr) {
        lv_obj_add_flag(objects.cover_image, LV_OBJ_FLAG_HIDDEN);
    }
    if (objects.cover_placeholder != nullptr) {
        lv_obj_remove_flag(objects.cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

void DetachHallCover(const HallCardObjects& objects, lv_image_dsc_t& descriptor) {
    if (descriptor.data != nullptr) {
        lv_image_cache_drop(&descriptor);
        lv_image_header_cache_drop(&descriptor);
        descriptor = {};
    }
    ShowHallCoverPlaceholder(objects);
}

void AttachHallCover(const HallCardLayout& layout, const HallCardObjects& objects, lv_image_dsc_t& descriptor,
                     const host_ui::HallCoverModel& cover) {
    if (objects.cover_image == nullptr || cover.data == nullptr || cover.width != static_cast<uint32_t>(layout.width) ||
        cover.height != static_cast<uint32_t>(layout.width) || cover.stride == 0U || cover.size == 0U) {
        ShowHallCoverPlaceholder(objects);
        return;
    }
    if (descriptor.data != nullptr && descriptor.data != cover.data) {
        lv_image_cache_drop(&descriptor);
        lv_image_header_cache_drop(&descriptor);
    }
    descriptor = {};
    descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    descriptor.header.cf = LV_COLOR_FORMAT_RGB888;
    descriptor.header.w = layout.width;
    descriptor.header.h = layout.width;
    descriptor.header.stride = cover.stride;
    descriptor.data_size = cover.size;
    descriptor.data = cover.data;
    lv_image_set_src(objects.cover_image, &descriptor);
    lv_obj_remove_flag(objects.cover_image, LV_OBJ_FLAG_HIDDEN);
    if (objects.cover_placeholder != nullptr) {
        // A decoded cover is opaque, so keeping the colored placeholder alive
        // underneath only lets it leak through a rounded edge or a one-pixel
        // panel alignment boundary. It can be restored by
        // ShowHallCoverPlaceholder() if this cover is detached later.
        lv_obj_add_flag(objects.cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace micropixel::platform::lvgl::square_common
