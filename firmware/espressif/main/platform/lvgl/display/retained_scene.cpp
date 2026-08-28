#include "platform/lvgl/display/retained_scene.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "platform/graphics/command_stream.hpp"

namespace micropixel::platform::lvgl {
namespace {

constexpr char kTag[] = "micropixel_display";

template <typename T>
bool ReadStruct(const uint8_t* bytes, uint32_t length, uint32_t offset, T& value) {
    if (offset > length || sizeof(T) > length - offset) {
        return false;
    }
    std::memcpy(&value, bytes + offset, sizeof(T));
    return true;
}

lv_color_format_t BitmapLvColorFormat(uint32_t pixel_format) {
    return pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? LV_COLOR_FORMAT_ARGB8888 : LV_COLOR_FORMAT_RGB888;
}

uint16_t RetainedObjectOpcode(uint16_t opcode) {
    if (opcode == MICROPIXEL_GRAPHICS_OP_BLEND_RECT) {
        return MICROPIXEL_GRAPHICS_OP_FILL_RECT;
    }
    if (opcode == MICROPIXEL_GRAPHICS_OP_BLEND_TEXTURE) {
        return MICROPIXEL_GRAPHICS_OP_DRAW_TEXTURE;
    }
    return opcode;
}

}  // namespace

#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
void RetainedScene::BindSurface(lv_display_t* display, DirectFramebufferAccess* framebuffers) {
    surface_.Bind(display, framebuffers, logical_width_, logical_height_);
}
#endif

bool RetainedScene::Initialize() {
    if (objects_ == nullptr) {
        objects_ = static_cast<RetainedObject*>(heap_caps_calloc(
            MICROPIXEL_GRAPHICS_MAX_DRAW_OPERATIONS, sizeof(RetainedObject), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (objects_ == nullptr) {
            ESP_LOGE(kTag, "failed to allocate retained-object pool in PSRAM");
            return false;
        }
        ESP_LOGI(kTag, "retained-object pool: %zu bytes in PSRAM",
                 sizeof(RetainedObject) * MICROPIXEL_GRAPHICS_MAX_DRAW_OPERATIONS);
    }
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
    if (!surface_.Initialize()) {
        heap_caps_free(objects_);
        objects_ = nullptr;
        return false;
    }
#endif
    return true;
}

void RetainedScene::Release() {
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
    // The DMA2D/PPA clients own scarce internal DMA descriptors.  Keep those
    // clients reserved across Guest sessions and release only per-App pixels.
    surface_.Reset();
#endif
    for (uint32_t index = 0U; index < object_count_; ++index) {
        ReleaseObjectFont(objects_[index]);
    }
    heap_caps_free(objects_);
    objects_ = nullptr;
    object_count_ = 0U;
    last_used_ = 0U;
    background_valid_ = false;
}

void RetainedScene::DiscardAllObjects() {
    for (uint32_t index = 0U; index < object_count_; ++index) {
        DiscardObject(objects_[index]);
    }
    object_count_ = 0U;
    last_used_ = 0U;
}

bool RetainedScene::TopologyChanged(const uint8_t* bytes, uint32_t length,
                                    const micropixel_graphics_command_header_t& header) const {
    uint32_t offset = sizeof(header);
    uint32_t used = 0U;
    for (uint32_t index = 0U; index < header.command_count; ++index) {
        micropixel_graphics_record_header_t record{};
        if (!ReadStruct(bytes, length, offset, record)) {
            return true;
        }
        const bool retained = record.opcode != MICROPIXEL_GRAPHICS_OP_CLEAR &&
                              record.opcode != MICROPIXEL_GRAPHICS_OP_PUSH_STATE &&
                              record.opcode != MICROPIXEL_GRAPHICS_OP_POP_STATE;
        if (retained) {
            if (used >= last_used_ || used >= object_count_ || objects_[used].object == nullptr ||
                objects_[used].opcode != RetainedObjectOpcode(record.opcode)) {
                return true;
            }
            ++used;
        }
        offset += record.size;
    }
    return used != last_used_;
}

void RetainedScene::DiscardObject(RetainedObject& slot) {
    if ((slot.opcode == MICROPIXEL_GRAPHICS_OP_DRAW_TEXTURE || slot.opcode == MICROPIXEL_GRAPHICS_OP_BLEND_TEXTURE) &&
        slot.image.data != nullptr) {
        lv_image_cache_drop(&slot.image);
    }
    if (slot.object != nullptr) {
        lv_obj_delete(slot.object);
    }
    ReleaseObjectFont(slot);
    slot = {};
}

void RetainedScene::ReleaseObjectFont(RetainedObject& slot) {
    if (slot.font_handle != 0U) {
        fonts_.ReleaseSceneFont(slot.font_handle);
        slot.font_handle = 0U;
        slot.font = nullptr;
    }
}

void RetainedScene::ForgetObjects() {
    if (objects_ == nullptr) {
        return;
    }
    for (uint32_t index = 0U; index < object_count_; ++index) {
        RetainedObject& slot = objects_[index];
        ReleaseObjectFont(slot);
        if ((slot.opcode == MICROPIXEL_GRAPHICS_OP_DRAW_TEXTURE ||
             slot.opcode == MICROPIXEL_GRAPHICS_OP_BLEND_TEXTURE) &&
            slot.image.data != nullptr) {
            lv_image_cache_drop(&slot.image);
        }
    }
    object_count_ = 0U;
    last_used_ = 0U;
}

RetainedScene::RetainedObject& RetainedScene::PrepareObject(uint32_t index, uint16_t opcode, lv_obj_t* frame,
                                                            int32_t frame_width, bool& changed, bool& order_dirty) {
    opcode = RetainedObjectOpcode(opcode);
    RetainedObject& slot = objects_[index];
    if (slot.object != nullptr && slot.opcode != opcode) {
        DiscardObject(slot);
        order_dirty = true;
    }
    if (slot.object == nullptr) {
        slot.opcode = opcode;
        slot.parent = frame;
        if (opcode == MICROPIXEL_GRAPHICS_OP_FILL_RECT || opcode == MICROPIXEL_GRAPHICS_OP_BLEND_RECT) {
            slot.object = lv_obj_create(frame);
            lv_obj_set_style_pad_all(slot.object, 0, 0);
            lv_obj_set_style_border_width(slot.object, 0, 0);
            lv_obj_set_style_radius(slot.object, 0, 0);
            lv_obj_set_style_bg_opa(slot.object, LV_OPA_COVER, 0);
        } else if (graphics::IsTextOpcode(opcode)) {
            slot.object = lv_label_create(frame);
            if (opcode == MICROPIXEL_GRAPHICS_OP_DRAW_TEXT_CENTERED) {
                lv_obj_set_width(slot.object, frame_width);
                lv_obj_set_style_text_align(slot.object, LV_TEXT_ALIGN_CENTER, 0);
            }
        } else {
            slot.object = lv_image_create(frame);
            lv_image_set_inner_align(slot.object, LV_IMAGE_ALIGN_TOP_LEFT);
            lv_image_set_pivot(slot.object, 0, 0);
            lv_image_set_antialias(slot.object, false);
            lv_obj_set_style_image_opa(slot.object, LV_OPA_COVER, 0);
        }
        lv_obj_remove_flag(slot.object, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(slot.object, LV_OBJ_FLAG_CLICKABLE);
        // New retained slots start hidden. The command-specific path updates all
        // properties first and only then exposes a real command. This also lets
        // fixed command streams reserve slots without painting a dummy pixel.
        lv_obj_add_flag(slot.object, LV_OBJ_FLAG_HIDDEN);
        slot.visible = false;
        changed = true;
        order_dirty = true;
    }
    if (slot.parent != frame) {
        lv_obj_set_parent(slot.object, frame);
        slot.parent = frame;
        slot.state_valid = false;
        if (opcode == MICROPIXEL_GRAPHICS_OP_DRAW_TEXT_CENTERED) {
            lv_obj_set_width(slot.object, frame_width);
        }
        changed = true;
        order_dirty = true;
    }
    if (index >= object_count_) {
        object_count_ = index + 1U;
    }
    return slot;
}

void RetainedScene::SetObjectVisible(RetainedObject& slot, bool visible, bool& changed) {
    if (slot.visible == visible) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(slot.object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(slot.object, LV_OBJ_FLAG_HIDDEN);
    }
    slot.visible = visible;
    changed = true;
}

bool RetainedScene::IsFillPlaceholder(const micropixel_graphics_fill_rect_command_t& command) const {
    return background_valid_ && command.width == 1 && command.height == 1 && command.rgb888 == background_rgb888_;
}

bool RetainedScene::IsTextPlaceholder(const micropixel_graphics_draw_text_command_t& command, const char* text) const {
    return command.text_length == 1U && text[0] == ' ';
}

RetainedFrameResult RetainedScene::Execute(const uint8_t* bytes, uint32_t length, lv_obj_t* frame,
                                           device::BitmapResolver resolver, void* resolver_context) {
    bool visual_changed = false;
    micropixel_graphics_command_header_t header{};
    (void)ReadStruct(bytes, length, 0U, header);
    if (TopologyChanged(bytes, length, header)) {
        DiscardAllObjects();
    }
    uint32_t offset = sizeof(header);
    uint32_t used = 0U;
    bool order_dirty = false;
    lv_obj_t* target_frame = frame;
    int32_t target_origin_x = 0;
    int32_t target_origin_y = 0;
    int32_t target_width = logical_width_;
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
    micropixel_graphics_push_state_command_t surface_request{};
    bool surface_seen = false;
#endif

    for (uint32_t index = 0U; index < header.command_count; ++index) {
        micropixel_graphics_record_header_t record{};
        (void)ReadStruct(bytes, length, offset, record);
        bool changed = false;
        if (record.opcode == MICROPIXEL_GRAPHICS_OP_PUSH_STATE) {
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
            micropixel_graphics_push_state_command_t command{};
            (void)ReadStruct(bytes, length, offset, command);
            if (!surface_.Configure(frame, command, background_valid_, background_rgb888_)) {
                return {MICROPIXEL_STATUS_RESOURCE_EXHAUSTED, false, surface_.Active()};
            }
            surface_request = command;
            surface_seen = true;
            target_frame = surface_.Frame();
            target_origin_x = command.clip_x;
            target_origin_y = command.clip_y;
            target_width = command.width;
#endif
            offset += record.size;
            continue;
        } else if (record.opcode == MICROPIXEL_GRAPHICS_OP_POP_STATE) {
            target_frame = frame;
            target_origin_x = 0;
            target_origin_y = 0;
            target_width = logical_width_;
            offset += record.size;
            continue;
        } else if (record.opcode == MICROPIXEL_GRAPHICS_OP_CLEAR) {
            micropixel_graphics_clear_command_t command{};
            (void)ReadStruct(bytes, length, offset, command);
            if (!background_valid_ || background_rgb888_ != command.rgb888) {
                lv_obj_set_style_bg_color(frame, lv_color_hex(command.rgb888), 0);
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
                surface_.SetBackground(command.rgb888);
#endif
                background_rgb888_ = command.rgb888;
                background_valid_ = true;
                changed = true;
            }
            used = 0U;
        } else if (record.opcode == MICROPIXEL_GRAPHICS_OP_FILL_RECT ||
                   record.opcode == MICROPIXEL_GRAPHICS_OP_BLEND_RECT) {
            micropixel_graphics_fill_rect_command_t fill_command{};
            micropixel_graphics_blend_rect_command_t blend_command{};
            int32_t x = 0;
            int32_t y = 0;
            int32_t width = 0;
            int32_t height = 0;
            uint32_t rgb888 = 0U;
            uint8_t opacity = LV_OPA_COVER;
            bool placeholder = false;
            if (record.opcode == MICROPIXEL_GRAPHICS_OP_FILL_RECT) {
                (void)ReadStruct(bytes, length, offset, fill_command);
                x = fill_command.x;
                y = fill_command.y;
                width = fill_command.width;
                height = fill_command.height;
                rgb888 = fill_command.rgb888;
                placeholder = IsFillPlaceholder(fill_command);
            } else {
                (void)ReadStruct(bytes, length, offset, blend_command);
                x = blend_command.x;
                y = blend_command.y;
                width = blend_command.width;
                height = blend_command.height;
                rgb888 = blend_command.rgb888;
                opacity = blend_command.opacity;
                placeholder = opacity == LV_OPA_TRANSP;
            }
            RetainedObject& slot =
                PrepareObject(used++, record.opcode, target_frame, target_width, changed, order_dirty);
            if (placeholder) {
                SetObjectVisible(slot, false, changed);
            } else {
                if (!slot.state_valid || slot.x != x || slot.y != y) {
                    lv_obj_set_pos(slot.object, x - target_origin_x, y - target_origin_y);
                    slot.x = x;
                    slot.y = y;
                    changed = true;
                }
                if (!slot.state_valid || slot.width != width || slot.height != height) {
                    lv_obj_set_size(slot.object, width, height);
                    slot.width = width;
                    slot.height = height;
                    changed = true;
                }
                if (!slot.state_valid || slot.rgb888 != rgb888) {
                    lv_obj_set_style_bg_color(slot.object, lv_color_hex(rgb888), 0);
                    slot.rgb888 = rgb888;
                    changed = true;
                }
                if (!slot.state_valid || slot.opacity != opacity) {
                    lv_obj_set_style_bg_opa(slot.object, opacity, 0);
                    slot.opacity = opacity;
                    changed = true;
                }
                slot.state_valid = true;
                SetObjectVisible(slot, true, changed);
            }
        } else if (graphics::IsTextOpcode(record.opcode)) {
            micropixel_graphics_draw_text_command_t command{};
            (void)ReadStruct(bytes, length, offset, command);
            RetainedObject& slot =
                PrepareObject(used++, record.opcode, target_frame, target_width, changed, order_dirty);
            const char* text = reinterpret_cast<const char*>(bytes + offset + sizeof(command));
            if (IsTextPlaceholder(command, text)) {
                ReleaseObjectFont(slot);
                SetObjectVisible(slot, false, changed);
            } else {
                if (slot.text_length != command.text_length || std::memcmp(slot.text, text, command.text_length) != 0) {
                    std::memcpy(slot.text, text, command.text_length);
                    slot.text[command.text_length] = '\0';
                    slot.text_length = command.text_length;
                    lv_label_set_text_static(slot.object, slot.text);
                    changed = true;
                }
                if (!slot.state_valid || slot.x != command.x || slot.y != command.y) {
                    const int32_t x = record.opcode == MICROPIXEL_GRAPHICS_OP_DRAW_TEXT_CENTERED
                                          ? command.x - target_origin_x - target_width / 2
                                          : command.x - target_origin_x;
                    lv_obj_set_pos(slot.object, x, command.y - target_origin_y);
                    slot.x = command.x;
                    slot.y = command.y;
                    changed = true;
                }
                if (!slot.state_valid || slot.rgb888 != command.rgb888) {
                    lv_obj_set_style_text_color(slot.object, lv_color_hex(command.rgb888), 0);
                    slot.rgb888 = command.rgb888;
                    changed = true;
                }
                const lv_font_t* font = fonts_.ResolveRetainedHandle(command.font_handle);
                if (slot.font_handle != command.font_handle) {
                    if (!fonts_.RetainSceneFont(command.font_handle)) {
                        return {MICROPIXEL_STATUS_RESOURCE_EXHAUSTED, false, SurfaceActive()};
                    }
                    font = fonts_.ResolveRetainedHandle(command.font_handle);
                    if (font == nullptr) {
                        fonts_.ReleaseSceneFont(command.font_handle);
                        return {MICROPIXEL_STATUS_INVALID_ARGUMENT, false, SurfaceActive()};
                    }
                    const micropixel_font_handle_t previous_font = slot.font_handle;
                    lv_obj_set_style_text_font(slot.object, font, 0);
                    slot.font = font;
                    slot.font_handle = command.font_handle;
                    fonts_.ReleaseSceneFont(previous_font);
                    changed = true;
                } else if (!slot.state_valid || slot.font != font) {
                    lv_obj_set_style_text_font(slot.object, font, 0);
                    slot.font = font;
                    changed = true;
                }
                slot.state_valid = true;
                SetObjectVisible(slot, true, changed);
            }
        } else {
            micropixel_graphics_draw_texture_command_t draw_command{};
            micropixel_graphics_blend_texture_command_t blend_command{};
            micropixel_texture_handle_t bitmap_handle = 0U;
            int32_t x = 0;
            int32_t y = 0;
            int32_t source_x = 0;
            int32_t source_y = 0;
            int32_t source_width = 0;
            int32_t source_height = 0;
            int32_t width = 0;
            int32_t height = 0;
            uint8_t opacity = LV_OPA_COVER;
            if (record.opcode == MICROPIXEL_GRAPHICS_OP_DRAW_TEXTURE) {
                (void)ReadStruct(bytes, length, offset, draw_command);
                bitmap_handle = draw_command.texture;
                x = draw_command.x;
                y = draw_command.y;
                source_x = draw_command.source_x;
                source_y = draw_command.source_y;
                source_width = draw_command.source_width;
                source_height = draw_command.source_height;
                width = draw_command.width;
                height = draw_command.height;
            } else {
                (void)ReadStruct(bytes, length, offset, blend_command);
                bitmap_handle = blend_command.texture;
                x = blend_command.x;
                y = blend_command.y;
                source_x = blend_command.source_x;
                source_y = blend_command.source_y;
                source_width = blend_command.source_width;
                source_height = blend_command.source_height;
                width = blend_command.width;
                height = blend_command.height;
                opacity = blend_command.opacity;
            }
            device::BitmapView bitmap{};
            if (!resolver(resolver_context, bitmap_handle, bitmap)) {
                return {MICROPIXEL_STATUS_INVALID_ARGUMENT, false, SurfaceActive()};
            }
            RetainedObject& slot =
                PrepareObject(used++, record.opcode, target_frame, target_width, changed, order_dirty);
            if (slot.texture != bitmap_handle || slot.image.data != bitmap.data ||
                slot.image.data_size != bitmap.size) {
                if (slot.image.data != nullptr) {
                    lv_image_cache_drop(&slot.image);
                }
                slot.image = {};
                slot.image.header.magic = LV_IMAGE_HEADER_MAGIC;
                slot.image.header.cf = BitmapLvColorFormat(bitmap.pixel_format);
                slot.image.header.w = bitmap.width;
                slot.image.header.h = bitmap.height;
                slot.image.header.stride = bitmap.stride;
                slot.image.data_size = bitmap.size;
                slot.image.data = bitmap.data;
                slot.texture = bitmap_handle;
                lv_image_set_src(slot.object, &slot.image);
                changed = true;
            }
            if (!slot.state_valid || slot.width != width || slot.height != height) {
                lv_obj_set_size(slot.object, width, height);
                slot.width = width;
                slot.height = height;
                changed = true;
            }
            uint32_t scale_x = static_cast<uint32_t>(
                (static_cast<uint64_t>(width) * LV_SCALE_NONE + static_cast<uint32_t>(source_width) / 2U) /
                static_cast<uint32_t>(source_width));
            uint32_t scale_y = static_cast<uint32_t>(
                (static_cast<uint64_t>(height) * LV_SCALE_NONE + static_cast<uint32_t>(source_height) / 2U) /
                static_cast<uint32_t>(source_height));
            scale_x = scale_x == 0U ? 1U : scale_x;
            scale_y = scale_y == 0U ? 1U : scale_y;
            const bool scale_changed = !slot.state_valid || slot.scale_x != scale_x || slot.scale_y != scale_y;
            if (scale_changed) {
                lv_image_set_scale_x(slot.object, scale_x);
                lv_image_set_scale_y(slot.object, scale_y);
                slot.scale_x = scale_x;
                slot.scale_y = scale_y;
                changed = true;
            }
            if (!slot.state_valid || slot.source_x != source_x || slot.source_y != source_y || scale_changed) {
                const int32_t offset_x = -static_cast<int32_t>(
                    (static_cast<int64_t>(source_x) * scale_x + LV_SCALE_NONE / 2U) / LV_SCALE_NONE);
                const int32_t offset_y = -static_cast<int32_t>(
                    (static_cast<int64_t>(source_y) * scale_y + LV_SCALE_NONE / 2U) / LV_SCALE_NONE);
                lv_image_set_offset_x(slot.object, offset_x);
                lv_image_set_offset_y(slot.object, offset_y);
                slot.source_x = source_x;
                slot.source_y = source_y;
                changed = true;
            }
            slot.source_width = source_width;
            slot.source_height = source_height;
            if (!slot.state_valid || slot.x != x || slot.y != y) {
                lv_obj_set_pos(slot.object, x - target_origin_x, y - target_origin_y);
                slot.x = x;
                slot.y = y;
                changed = true;
            }
            if (!slot.state_valid || slot.opacity != opacity) {
                lv_obj_set_style_image_opa(slot.object, opacity, 0);
                slot.opacity = opacity;
                changed = true;
            }
            slot.state_valid = true;
            SetObjectVisible(slot, opacity != LV_OPA_TRANSP, changed);
        }
        visual_changed = visual_changed || changed;
        offset += record.size;
    }

    for (uint32_t index = used; index < object_count_; ++index) {
        RetainedObject& slot = objects_[index];
        ReleaseObjectFont(slot);
        if (slot.visible) {
            bool changed = false;
            SetObjectVisible(slot, false, changed);
            visual_changed = visual_changed || changed;
        }
    }
    if (order_dirty) {
        for (uint32_t index = 0U; index < used; ++index) {
            lv_obj_move_to_index(objects_[index].object, static_cast<int32_t>(index));
        }
        // A newly created or reparented LVGL object has no final coordinates
        // until layout runs. Its automatic invalidation can therefore be an
        // empty area, which is invisible when the next Present contains only
        // unrelated bitmap damage. Resolve the retained topology first, then
        // invalidate each visible object at its real bounds exactly once.
        lv_obj_update_layout(frame);
        for (uint32_t index = 0U; index < used; ++index) {
            if (objects_[index].visible) {
                lv_obj_invalidate(objects_[index].object);
            }
        }
    }
    last_used_ = used;
    bool surface_changed = false;
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
    const bool surface_was_active = surface_.Active();
    surface_changed = surface_.Update(surface_seen ? &surface_request : nullptr);
    if (surface_was_active && !surface_.Active()) {
        // LVGL can consume object invalidations while dummy draw is active,
        // even though those updates never reach the panel framebuffers.  Once
        // direct composition ends, invalidate the current retained tree so
        // its latest state replaces the frozen surface image immediately.
        lv_obj_invalidate(frame);
        surface_changed = true;
    }
#endif
    visual_changed = visual_changed || surface_changed;
    return {MICROPIXEL_STATUS_OK, visual_changed, SurfaceActive()};
}

bool RetainedScene::InvalidateBitmap(const uint8_t* data, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (data == nullptr || width == 0U || height == 0U) {
        return false;
    }
    const uint64_t dirty_right = static_cast<uint64_t>(x) + width;
    const uint64_t dirty_bottom = static_cast<uint64_t>(y) + height;
    bool invalidated = false;
    for (uint32_t index = 0U; index < last_used_; ++index) {
        RetainedObject& slot = objects_[index];
        if (!slot.visible || slot.image.data != data ||
            (slot.opcode != MICROPIXEL_GRAPHICS_OP_DRAW_TEXTURE &&
             slot.opcode != MICROPIXEL_GRAPHICS_OP_BLEND_TEXTURE)) {
            continue;
        }
        const uint32_t source_left = static_cast<uint32_t>(slot.source_x);
        const uint32_t source_top = static_cast<uint32_t>(slot.source_y);
        const uint64_t source_right = static_cast<uint64_t>(source_left) + static_cast<uint32_t>(slot.source_width);
        const uint64_t source_bottom = static_cast<uint64_t>(source_top) + static_cast<uint32_t>(slot.source_height);
        const uint32_t clipped_left = x > source_left ? x : source_left;
        const uint32_t clipped_top = y > source_top ? y : source_top;
        const uint64_t clipped_right = dirty_right < source_right ? dirty_right : source_right;
        const uint64_t clipped_bottom = dirty_bottom < source_bottom ? dirty_bottom : source_bottom;
        if (clipped_right <= clipped_left || clipped_bottom <= clipped_top) {
            continue;
        }
        lv_area_t object_area{};
        lv_obj_get_coords(slot.object, &object_area);
        lv_area_t dirty_area{
            .x1 = object_area.x1 + static_cast<int32_t>((clipped_left - source_left) * slot.width /
                                                        static_cast<uint32_t>(slot.source_width)),
            .y1 = object_area.y1 + static_cast<int32_t>((clipped_top - source_top) * slot.height /
                                                        static_cast<uint32_t>(slot.source_height)),
            .x2 = object_area.x1 +
                  static_cast<int32_t>(
                      ((clipped_right - source_left) * slot.width + static_cast<uint32_t>(slot.source_width) - 1U) /
                      static_cast<uint32_t>(slot.source_width)) -
                  1,
            .y2 = object_area.y1 +
                  static_cast<int32_t>(
                      ((clipped_bottom - source_top) * slot.height + static_cast<uint32_t>(slot.source_height) - 1U) /
                      static_cast<uint32_t>(slot.source_height)) -
                  1,
        };
        (void)lv_obj_invalidate_area(slot.object, &dirty_area);
        invalidated = true;
    }
    return invalidated;
}

bool RetainedScene::SurfaceActive() const {
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
    return surface_.Active();
#else
    return false;
#endif
}

}  // namespace micropixel::platform::lvgl
