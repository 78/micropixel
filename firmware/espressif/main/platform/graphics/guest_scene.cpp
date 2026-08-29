#include "platform/graphics/guest_scene.hpp"

#include <cstdint>
#include <cstring>
#include <utility>

#include "abi/micropixel_abi.h"
#include "device/text.hpp"

namespace micropixel::platform::graphics {
namespace {

constexpr uint32_t kNodeBaseMask = MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE |
                                   MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY | MICROPIXEL_GRAPHICS_SCENE_NODE_LAYER;
constexpr uint32_t kNodeVisualMask = kNodeBaseMask | MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY;
constexpr uint32_t kRectMask = kNodeVisualMask;
constexpr uint32_t kTextureMask = kNodeVisualMask | MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT;
constexpr uint32_t kTextMask = kNodeVisualMask | MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT;
constexpr uint32_t kSpriteBatchMask = kNodeBaseMask | MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT;
constexpr uint32_t kLayerMask = MICROPIXEL_GRAPHICS_SCENE_LAYER_CLIP | MICROPIXEL_GRAPHICS_SCENE_LAYER_TRANSLATION |
                                MICROPIXEL_GRAPHICS_SCENE_LAYER_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_LAYER_Z_ORDER;
constexpr uint32_t kInstanceMask =
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_GEOMETRY | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_CONTENT |
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBILITY;

template <typename Value>
bool Read(const uint8_t* bytes, uint32_t length, uint32_t offset, Value& value) {
    if (bytes == nullptr || offset > length || sizeof(Value) > length - offset) {
        return false;
    }
    std::memcpy(&value, bytes + offset, sizeof(Value));
    return true;
}

bool ValidRgb888(uint32_t color) { return (color & 0xff000000U) == 0U; }

bool ValidRect(int32_t x, int32_t y, int32_t width, int32_t height, int32_t logical_width, int32_t logical_height) {
    return x >= 0 && y >= 0 && width > 0 && height > 0 && static_cast<int64_t>(x) + width <= logical_width &&
           static_cast<int64_t>(y) + height <= logical_height;
}

uint32_t BitmapBytesPerPixel(uint32_t format) {
    if (format == MICROPIXEL_PIXEL_FORMAT_BGR888) {
        return 3U;
    }
    return format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? 4U : 0U;
}

bool ValidBitmap(const device::BitmapView& bitmap) {
    const uint32_t bytes_per_pixel = BitmapBytesPerPixel(bitmap.pixel_format);
    if (bitmap.data == nullptr || bitmap.width == 0U || bitmap.height == 0U || bytes_per_pixel == 0U ||
        bitmap.width > UINT32_MAX / bytes_per_pixel) {
        return false;
    }
    const uint32_t row_bytes = bitmap.width * bytes_per_pixel;
    return bitmap.stride >= row_bytes && bitmap.stride % bytes_per_pixel == 0U &&
           static_cast<uint64_t>(bitmap.stride) * bitmap.height <= bitmap.size;
}

uint32_t RequiredMask(uint16_t opcode) {
    if (opcode == MICROPIXEL_GRAPHICS_SCENE_OP_RECT) {
        return kRectMask;
    }
    if (opcode == MICROPIXEL_GRAPHICS_SCENE_OP_TEXTURE) {
        return kTextureMask;
    }
    if (opcode == MICROPIXEL_GRAPHICS_SCENE_OP_TEXT) {
        return kTextMask;
    }
    return opcode == MICROPIXEL_GRAPHICS_SCENE_OP_SPRITE_BATCH ? kSpriteBatchMask : 0U;
}

GuestSceneNodeKind NodeKind(uint16_t opcode) {
    if (opcode == MICROPIXEL_GRAPHICS_SCENE_OP_TEXTURE) {
        return GuestSceneNodeKind::kTexture;
    }
    if (opcode == MICROPIXEL_GRAPHICS_SCENE_OP_TEXT) {
        return GuestSceneNodeKind::kText;
    }
    return opcode == MICROPIXEL_GRAPHICS_SCENE_OP_SPRITE_BATCH ? GuestSceneNodeKind::kSpriteBatch
                                                               : GuestSceneNodeKind::kRect;
}

bool SameKind(GuestSceneNodeKind kind, uint16_t opcode) { return kind == NodeKind(opcode); }

bool ZeroPadding(const uint8_t* bytes, uint32_t begin, uint32_t end) {
    for (uint32_t offset = begin; offset < end; ++offset) {
        if (bytes[offset] != 0U) {
            return false;
        }
    }
    return true;
}

}  // namespace

int32_t GuestScene::Apply(const uint8_t* bytes, uint32_t length, int32_t logical_width, int32_t logical_height,
                          device::BitmapResolver bitmap_resolver, void* bitmap_context,
                          device::FontValidator font_validator, void* font_context) {
    micropixel_graphics_scene_header_t header{};
    if (capacity_ == 0U || capacity_ > MICROPIXEL_GRAPHICS_MAX_SCENE_NODES || instance_capacity_ == 0U ||
        instance_capacity_ > MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES || current_ == nullptr || scratch_ == nullptr ||
        current_instances_ == nullptr || scratch_instances_ == nullptr || logical_width <= 0 || logical_height <= 0 ||
        !Read(bytes, length, 0U, header) || header.magic != MICROPIXEL_GRAPHICS_SCENE_MAGIC ||
        header.interface_major != MICROPIXEL_GRAPHICS_INTERFACE_MAJOR ||
        header.interface_minor > MICROPIXEL_GRAPHICS_INTERFACE_MINOR || header.flags != 0U ||
        header.total_size != length || header.node_count > capacity_ ||
        header.batch_instance_count > instance_capacity_ ||
        static_cast<uint32_t>(header.node_count) + header.batch_instance_count > MICROPIXEL_GRAPHICS_MAX_SCENE_NODES ||
        header.layer_count > MICROPIXEL_GRAPHICS_MAX_LAYERS) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const bool keyframe = header.kind == MICROPIXEL_GRAPHICS_SCENE_KEYFRAME;
    const bool patch = header.kind == MICROPIXEL_GRAPHICS_SCENE_PATCH;
    if ((!keyframe && !patch) ||
        (keyframe && (header.generation == 0U || header.base_revision != 0U || header.revision != 1U)) ||
        (patch &&
         (!valid_ || revision_ == UINT32_MAX || header.generation != generation_ || header.base_revision != revision_ ||
          header.revision != revision_ + 1U || header.node_count != node_count_ || header.layer_count != layer_count_ ||
          header.batch_instance_count != batch_instance_count_))) {
        return patch ? MICROPIXEL_STATUS_STALE_STATE : MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }

    std::memset(node_changes_, 0, sizeof(node_changes_));
    std::memset(layer_changes_, 0, sizeof(layer_changes_));
    std::memset(instance_changes_, 0, sizeof(instance_changes_));
    last_apply_was_keyframe_ = false;
    background_changed_ = false;

    uint16_t scratch_node_count = header.node_count;
    uint16_t scratch_layer_count = header.layer_count;
    uint16_t scratch_batch_instance_count = header.batch_instance_count;
    uint32_t scratch_background = background_rgb888_;
    if (keyframe) {
        for (uint16_t index = 0U; index < capacity_; ++index) {
            scratch_[index] = {};
        }
        for (GuestSceneLayer& layer : scratch_layers_) {
            layer = {};
        }
        for (uint16_t index = 0U; index < instance_capacity_; ++index) {
            scratch_instances_[index] = {};
        }
        scratch_background = 0U;
    } else {
        for (uint16_t index = 0U; index < node_count_; ++index) {
            scratch_[index] = current_[index];
        }
        for (uint16_t index = 0U; index <= MICROPIXEL_GRAPHICS_MAX_LAYERS; ++index) {
            scratch_layers_[index] = layers_[index];
        }
        for (uint16_t index = 0U; index < batch_instance_count_; ++index) {
            scratch_instances_[index] = current_instances_[index];
        }
    }

    bool background_seen = false;
    bool node_seen[MICROPIXEL_GRAPHICS_MAX_SCENE_NODES]{};
    bool layer_seen[MICROPIXEL_GRAPHICS_MAX_LAYERS + 1U]{};
    bool instance_seen[MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES]{};
    uint16_t next_instance_offset = 0U;
    uint8_t batch_count = 0U;
    uint32_t offset = sizeof(header);
    for (uint16_t record_index = 0U; record_index < header.record_count; ++record_index) {
        micropixel_graphics_scene_record_header_t record{};
        if (!Read(bytes, length, offset, record) || record.size < sizeof(record) || (record.size & 3U) != 0U ||
            record.size > length - offset) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        if (record.opcode == MICROPIXEL_GRAPHICS_SCENE_OP_BACKGROUND) {
            micropixel_graphics_scene_background_record_t value{};
            if (background_seen || record.size != sizeof(value) || !Read(bytes, length, offset, value) ||
                value.property_mask != MICROPIXEL_GRAPHICS_SCENE_BACKGROUND_COLOR || !ValidRgb888(value.rgb888)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            scratch_background = value.rgb888;
            background_seen = true;
            background_changed_ = true;
        } else if (record.opcode == MICROPIXEL_GRAPHICS_SCENE_OP_LAYER) {
            micropixel_graphics_scene_layer_record_t value{};
            if (record.size != sizeof(value) || !Read(bytes, length, offset, value) || value.layer_id == 0U ||
                value.layer_id > scratch_layer_count || value.reserved0 != 0U || layer_seen[value.layer_id] ||
                value.property_mask == 0U || (value.property_mask & ~kLayerMask) != 0U ||
                (keyframe && value.property_mask != kLayerMask)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            GuestSceneLayer& layer = scratch_layers_[value.layer_id];
            if ((value.property_mask & MICROPIXEL_GRAPHICS_SCENE_LAYER_CLIP) != 0U) {
                layer.clip_x = value.clip_x;
                layer.clip_y = value.clip_y;
                layer.width = value.width;
                layer.height = value.height;
            }
            if ((value.property_mask & MICROPIXEL_GRAPHICS_SCENE_LAYER_TRANSLATION) != 0U) {
                layer.translate_x = value.translate_x;
                layer.translate_y = value.translate_y;
            }
            if ((value.property_mask & MICROPIXEL_GRAPHICS_SCENE_LAYER_APPEARANCE) != 0U) {
                if (value.visible > 1U) {
                    return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                }
                layer.opacity = value.opacity;
                layer.visible = value.visible != 0U;
            }
            if ((value.property_mask & MICROPIXEL_GRAPHICS_SCENE_LAYER_Z_ORDER) != 0U) {
                layer.z_order = value.z_order;
            }
            layer_seen[value.layer_id] = true;
            layer_changes_[value.layer_id] = static_cast<uint8_t>(value.property_mask);
        } else if (record.opcode == MICROPIXEL_GRAPHICS_SCENE_OP_BATCH_INSTANCES) {
            micropixel_graphics_scene_batch_instances_record_t value{};
            if (!Read(bytes, length, offset, value) || value.batch_node_id >= scratch_node_count ||
                value.instance_count == 0U || value.reserved0 != 0U || value.property_mask == 0U ||
                (value.property_mask & ~kInstanceMask) != 0U || (keyframe && value.property_mask != kInstanceMask)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            const uint32_t expected_size = sizeof(value) + static_cast<uint32_t>(value.instance_count) *
                                                               sizeof(micropixel_graphics_scene_sprite_instance_t);
            GuestSceneNode& batch = scratch_[value.batch_node_id];
            if (record.size != expected_size || batch.kind != GuestSceneNodeKind::kSpriteBatch ||
                static_cast<uint32_t>(value.first_instance) + value.instance_count > batch.batch_capacity) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            for (uint16_t instance = 0U; instance < value.instance_count; ++instance) {
                const uint16_t batch_instance = value.first_instance + instance;
                const uint16_t scene_instance = batch.batch_instance_offset + batch_instance;
                micropixel_graphics_scene_sprite_instance_t wire{};
                if (scene_instance >= scratch_batch_instance_count || instance_seen[scene_instance] ||
                    !Read(bytes, length,
                          offset + sizeof(value) +
                              static_cast<uint32_t>(instance) * sizeof(micropixel_graphics_scene_sprite_instance_t),
                          wire) ||
                    wire.reserved0 != 0U || (wire.flags & ~MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBLE) != 0U) {
                    return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                }
                GuestSceneSpriteInstance& target = scratch_instances_[scene_instance];
                if ((value.property_mask & MICROPIXEL_GRAPHICS_SCENE_INSTANCE_GEOMETRY) != 0U) {
                    target.x = wire.x;
                    target.y = wire.y;
                    target.width = wire.width;
                    target.height = wire.height;
                }
                if ((value.property_mask & MICROPIXEL_GRAPHICS_SCENE_INSTANCE_CONTENT) != 0U) {
                    target.source_x = wire.source_x;
                    target.source_y = wire.source_y;
                    target.source_width = wire.source_width;
                    target.source_height = wire.source_height;
                }
                if ((value.property_mask & MICROPIXEL_GRAPHICS_SCENE_INSTANCE_APPEARANCE) != 0U) {
                    target.rgb888 = wire.rgb888;
                    target.opacity = wire.opacity;
                }
                if ((value.property_mask & MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBILITY) != 0U) {
                    target.flags = static_cast<uint8_t>(wire.flags & MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBLE);
                }
                instance_seen[scene_instance] = true;
                instance_changes_[scene_instance] = static_cast<uint8_t>(value.property_mask);
            }
        } else {
            micropixel_graphics_scene_node_header_t node_header{};
            const uint32_t required_mask = RequiredMask(record.opcode);
            if (required_mask == 0U || !Read(bytes, length, offset, node_header) ||
                node_header.node_id >= scratch_node_count || node_header.layer_id > scratch_layer_count ||
                node_seen[node_header.node_id] || node_header.property_mask == 0U ||
                (node_header.property_mask & ~(required_mask | MICROPIXEL_GRAPHICS_SCENE_NODE_KIND)) != 0U ||
                (keyframe && node_header.property_mask != (required_mask | MICROPIXEL_GRAPHICS_SCENE_NODE_KIND))) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            GuestSceneNode& node = scratch_[node_header.node_id];
            const bool replacing = keyframe || !SameKind(node.kind, record.opcode);
            if (replacing && (node_header.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_KIND) == 0U) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            if (replacing && (node_header.property_mask & required_mask) != required_mask) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            if (!replacing && (node_header.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_KIND) != 0U) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            if (replacing) {
                node = {};
                node.kind = NodeKind(record.opcode);
            }
            if ((node_header.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_LAYER) != 0U) {
                node.layer_id = node_header.layer_id;
            }
            if ((node_header.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY) != 0U) {
                node.visible = (node_header.flags & MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE) != 0U;
            }

            if (record.opcode == MICROPIXEL_GRAPHICS_SCENE_OP_RECT) {
                micropixel_graphics_scene_rect_record_t value{};
                if (record.size != sizeof(value) || !Read(bytes, length, offset, value) ||
                    (value.node.flags & ~MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE) != 0U || value.reserved0[0] != 0U ||
                    value.reserved0[1] != 0U || value.reserved0[2] != 0U) {
                    return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                }
                if ((value.node.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY) != 0U) {
                    node.x = value.x;
                    node.y = value.y;
                    node.width = value.width;
                    node.height = value.height;
                }
                if ((value.node.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE) != 0U) {
                    node.rgb888 = value.rgb888;
                    node.opacity = value.opacity;
                }
            } else if (record.opcode == MICROPIXEL_GRAPHICS_SCENE_OP_TEXTURE) {
                micropixel_graphics_scene_texture_record_t value{};
                if (record.size != sizeof(value) || !Read(bytes, length, offset, value) ||
                    (value.node.flags & ~MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE) != 0U || value.reserved0[0] != 0U ||
                    value.reserved0[1] != 0U || value.reserved0[2] != 0U) {
                    return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                }
                if ((value.node.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY) != 0U) {
                    node.x = value.x;
                    node.y = value.y;
                    node.width = value.width;
                    node.height = value.height;
                }
                if ((value.node.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE) != 0U) {
                    node.opacity = value.opacity;
                }
                if ((value.node.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT) != 0U) {
                    node.texture = value.texture;
                    node.source_x = value.source_x;
                    node.source_y = value.source_y;
                    node.source_width = value.source_width;
                    node.source_height = value.source_height;
                }
            } else if (record.opcode == MICROPIXEL_GRAPHICS_SCENE_OP_SPRITE_BATCH) {
                micropixel_graphics_scene_sprite_batch_record_t value{};
                if (record.size != sizeof(value) || !Read(bytes, length, offset, value) || value.reserved0 != 0U ||
                    (value.node.flags & ~MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE) != 0U || value.capacity == 0U ||
                    value.capacity > MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES) {
                    return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                }
                if (keyframe) {
                    if (batch_count >= MICROPIXEL_GRAPHICS_MAX_SPRITE_BATCHES ||
                        static_cast<uint32_t>(next_instance_offset) + value.capacity > scratch_batch_instance_count) {
                        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                    }
                    node.batch_instance_offset = next_instance_offset;
                    node.batch_capacity = value.capacity;
                    next_instance_offset += value.capacity;
                    ++batch_count;
                } else if (value.capacity != node.batch_capacity) {
                    return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                }
                if ((value.node.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE) != 0U) {
                    node.opacity = value.opacity;
                }
                if ((value.node.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT) != 0U) {
                    node.texture = value.texture;
                }
            } else {
                micropixel_graphics_scene_text_record_t value{};
                if (!Read(bytes, length, offset, value)) {
                    return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                }
                const uint32_t expected_size = (sizeof(value) + value.text_length + 3U) & ~3U;
                if (record.size < sizeof(value) ||
                    (value.node.flags &
                     ~(MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE | MICROPIXEL_GRAPHICS_SCENE_TEXT_CENTERED)) != 0U ||
                    value.text_length > MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES || record.size != expected_size ||
                    !ZeroPadding(bytes, offset + sizeof(value) + value.text_length, offset + record.size)) {
                    return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                }
                if ((value.node.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY) != 0U) {
                    node.x = value.x;
                    node.y = value.y;
                    node.text_centered = (value.node.flags & MICROPIXEL_GRAPHICS_SCENE_TEXT_CENTERED) != 0U;
                }
                if ((value.node.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE) != 0U) {
                    node.rgb888 = value.rgb888;
                }
                if ((value.node.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT) != 0U) {
                    if (value.text_length == 0U ||
                        !device::IsValidUtf8(bytes + offset + sizeof(value), value.text_length)) {
                        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                    }
                    node.font = value.font;
                    node.text_length = value.text_length;
                    std::memcpy(node.text, bytes + offset + sizeof(value), value.text_length);
                    node.text[value.text_length] = '\0';
                }
            }
            node_seen[node_header.node_id] = true;
            node_changes_[node_header.node_id] = static_cast<uint8_t>(node_header.property_mask);
        }
        offset += record.size;
    }
    if (offset != length || (keyframe && !background_seen)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (keyframe) {
        if (next_instance_offset != scratch_batch_instance_count) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        for (uint16_t node = 0U; node < scratch_node_count; ++node) {
            if (!node_seen[node]) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
        }
        for (uint16_t layer = 1U; layer <= scratch_layer_count; ++layer) {
            if (!layer_seen[layer]) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
        }
        for (uint16_t instance = 0U; instance < scratch_batch_instance_count; ++instance) {
            if (!instance_seen[instance]) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
        }
    }
    if (!ValidateResult(logical_width, logical_height, bitmap_resolver, bitmap_context, font_validator, font_context,
                        scratch_node_count, scratch_layer_count, scratch_batch_instance_count)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }

    std::swap(current_, scratch_);
    std::swap(current_instances_, scratch_instances_);
    for (uint16_t index = 0U; index <= MICROPIXEL_GRAPHICS_MAX_LAYERS; ++index) {
        layers_[index] = scratch_layers_[index];
    }
    node_count_ = scratch_node_count;
    layer_count_ = scratch_layer_count;
    batch_instance_count_ = scratch_batch_instance_count;
    background_rgb888_ = scratch_background;
    generation_ = header.generation;
    revision_ = header.revision;
    last_apply_was_keyframe_ = keyframe;
    valid_ = true;
    return MICROPIXEL_STATUS_OK;
}

bool GuestScene::ValidateResult(int32_t logical_width, int32_t logical_height, device::BitmapResolver bitmap_resolver,
                                void* bitmap_context, device::FontValidator font_validator, void* font_context,
                                uint16_t node_count, uint16_t layer_count, uint16_t batch_instance_count) const {
    for (uint16_t index = 1U; index <= layer_count; ++index) {
        const GuestSceneLayer& layer = scratch_layers_[index];
        if (!ValidRect(layer.clip_x, layer.clip_y, layer.width, layer.height, logical_width, logical_height) ||
            layer.translate_x < -32 || layer.translate_x > 32 || layer.translate_y < -32 || layer.translate_y > 32 ||
            !ValidRect(layer.clip_x + layer.translate_x, layer.clip_y + layer.translate_y, layer.width, layer.height,
                       logical_width, logical_height)) {
            return false;
        }
    }
    for (uint16_t index = 0U; index < node_count; ++index) {
        const GuestSceneNode& node = scratch_[index];
        if (node.layer_id > layer_count) {
            return false;
        }
        if (node.kind == GuestSceneNodeKind::kRect) {
            if (!ValidRect(node.x, node.y, node.width, node.height, logical_width, logical_height) ||
                !ValidRgb888(node.rgb888)) {
                return false;
            }
        } else if (node.kind == GuestSceneNodeKind::kTexture) {
            device::BitmapView bitmap{};
            if (!ValidRect(node.x, node.y, node.width, node.height, logical_width, logical_height) ||
                node.texture == 0U || node.source_x < 0 || node.source_y < 0 || node.source_width <= 0 ||
                node.source_height <= 0 || bitmap_resolver == nullptr ||
                !bitmap_resolver(bitmap_context, node.texture, bitmap) || !ValidBitmap(bitmap) ||
                static_cast<int64_t>(node.source_x) + node.source_width > bitmap.width ||
                static_cast<int64_t>(node.source_y) + node.source_height > bitmap.height) {
                return false;
            }
        } else if (node.kind == GuestSceneNodeKind::kSpriteBatch) {
            device::BitmapView bitmap{};
            const bool textured = node.texture != 0U;
            if (node.batch_capacity == 0U ||
                static_cast<uint32_t>(node.batch_instance_offset) + node.batch_capacity > batch_instance_count ||
                (textured && (bitmap_resolver == nullptr || !bitmap_resolver(bitmap_context, node.texture, bitmap) ||
                              !ValidBitmap(bitmap)))) {
                return false;
            }
            for (uint16_t instance_index = 0U; instance_index < node.batch_capacity; ++instance_index) {
                const GuestSceneSpriteInstance& instance =
                    scratch_instances_[node.batch_instance_offset + instance_index];
                if ((instance.flags & MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBLE) == 0U) {
                    continue;
                }
                if (!ValidRect(instance.x, instance.y, instance.width, instance.height, logical_width,
                               logical_height) ||
                    !ValidRgb888(instance.rgb888) ||
                    (textured && (instance.source_x < 0 || instance.source_y < 0 || instance.source_width <= 0 ||
                                  instance.source_height <= 0 ||
                                  static_cast<int64_t>(instance.source_x) + instance.source_width > bitmap.width ||
                                  static_cast<int64_t>(instance.source_y) + instance.source_height > bitmap.height))) {
                    return false;
                }
            }
        } else if (node.x < 0 || node.x > logical_width || node.y < 0 || node.y >= logical_height ||
                   !ValidRgb888(node.rgb888) || node.font == 0U || node.text_length == 0U ||
                   node.text_length > MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES || font_validator == nullptr ||
                   !font_validator(font_context, node.font) ||
                   !device::IsValidUtf8(reinterpret_cast<const uint8_t*>(node.text), node.text_length)) {
            return false;
        }
    }
    return true;
}

void GuestScene::Reset() {
    node_count_ = 0U;
    layer_count_ = 0U;
    batch_instance_count_ = 0U;
    background_rgb888_ = 0U;
    generation_ = 0U;
    revision_ = 0U;
    std::memset(node_changes_, 0, sizeof(node_changes_));
    std::memset(layer_changes_, 0, sizeof(layer_changes_));
    std::memset(instance_changes_, 0, sizeof(instance_changes_));
    last_apply_was_keyframe_ = false;
    background_changed_ = false;
    valid_ = false;
    for (GuestSceneLayer& layer : layers_) {
        layer = {};
    }
    for (GuestSceneLayer& layer : scratch_layers_) {
        layer = {};
    }
}

}  // namespace micropixel::platform::graphics
