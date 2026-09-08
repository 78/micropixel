#include "platform/graphics/guest_scene.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

#include "abi/micropixel_abi.h"
#include "device/contracts/graphics.hpp"
#include "device/text.hpp"

namespace micropixel::platform::graphics {
namespace {

// First minor using the current Scene layout. Keep this baseline when adding
// backward-compatible Graphics features; it is not the Host current minor.

constexpr uint32_t kNodeBaseMask =
    MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY;
constexpr uint32_t kNodeVisualMask = kNodeBaseMask | MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY;
constexpr uint32_t kRectMask = kNodeVisualMask;
constexpr uint32_t kRoundedRectMask = kNodeVisualMask;
constexpr uint32_t kTextureMask = kNodeVisualMask | MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT;
constexpr uint32_t kTextMask = kNodeVisualMask | MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT;
constexpr uint32_t kSpriteBatchMask = kNodeBaseMask | MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT;
constexpr uint32_t kContainerMask =
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_CLIP | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION |
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_Z_ORDER |
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_STRUCTURE | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAGS;
constexpr uint16_t kKnownContainerFlags = MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAG_CACHED_CONTENT;
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

bool ValidClippedLocalRect(int32_t x, int32_t y, int32_t width, int32_t height) {
    const int64_t right = static_cast<int64_t>(x) + width;
    const int64_t bottom = static_cast<int64_t>(y) + height;
    return width > 0 && height > 0 && right >= INT32_MIN && right <= INT32_MAX && bottom >= INT32_MIN &&
           bottom <= INT32_MAX;
}

uint32_t BitmapBytesPerPixel(uint32_t format) {
    if (format == MICROPIXEL_PIXEL_FORMAT_BGR888) {
        return 3U;
    }
    if (format == MICROPIXEL_PIXEL_FORMAT_BGRA8888) {
        return 4U;
    }
    return format == MICROPIXEL_PIXEL_FORMAT_RGB565 ? 2U : 0U;
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

bool NormalizeRoundedTextureExtent(int32_t origin, int32_t& extent, uint32_t limit) {
    if (origin < 0 || extent <= 0 || limit == 0U) {
        return false;
    }
    const int64_t far_edge = static_cast<int64_t>(origin) + extent;
    if (far_edge <= limit) {
        return true;
    }
    // Older Guest SDKs independently rounded an adaptive atlas frame's origin
    // and extent. Two half-pixel round-ups can exceed the decoded far edge by
    // exactly one pixel even though the authored source rectangle was valid.
    if (far_edge == static_cast<int64_t>(limit) + 1 && extent > 1) {
        --extent;
        return true;
    }
    return false;
}

uint32_t RequiredMask(uint16_t opcode) {
    if (opcode == MICROPIXEL_GRAPHICS_SCENE_OP_RECT) {
        return kRectMask;
    }
    if (opcode == MICROPIXEL_GRAPHICS_SCENE_OP_ROUNDED_RECT) {
        return kRoundedRectMask;
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
    if (opcode == MICROPIXEL_GRAPHICS_SCENE_OP_ROUNDED_RECT) {
        return GuestSceneNodeKind::kRoundedRect;
    }
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

void GuestScene::RebindStorage(GuestSceneStorageView storage) {
    const auto copy = [](auto* destination, const auto* source, size_t count) {
        if (count != 0U) {
            std::copy_n(source, count, destination);
        }
    };
    copy(storage.nodes.data(), current_, node_count_);
    copy(storage.instances.data(), current_instances_, batch_instance_count_);
    if (containers_ != nullptr) {
        copy(storage.containers.data(), containers_, static_cast<size_t>(container_count_) + 1U);
        copy(storage.container_changes.data(), container_changes_, static_cast<size_t>(container_count_) + 1U);
    }
    copy(storage.draw_order.data(), draw_node_order_, node_count_);
    copy(storage.node_changes.data(), node_changes_, node_count_);
    copy(storage.instance_changes.data(), instance_changes_, batch_instance_count_);
    // Live text is rewritten into the front of the new arena; node offsets in
    // the new node array are updated as it goes.
    char* text = storage.text.data();
    uint32_t used = 0U;
    for (uint16_t index = 0U; index < node_count_; ++index) {
        GuestSceneNode& node = storage.nodes[index];
        if (node.kind != GuestSceneNodeKind::kText) {
            continue;
        }
        std::memcpy(text + used, text_ + node.text_offset, static_cast<size_t>(node.text_length) + 1U);
        node.text_offset = used;
        used += static_cast<uint32_t>(node.text_length) + 1U;
    }
    capacity_ = static_cast<uint16_t>(storage.nodes.size() / 2U);
    instance_capacity_ = static_cast<uint16_t>(storage.instances.size() / 2U);
    container_capacity_ = static_cast<uint16_t>(storage.containers.size() / 2U - 1U);
    current_ = storage.nodes.data();
    scratch_ = current_ + capacity_;
    current_instances_ = storage.instances.data();
    scratch_instances_ = current_instances_ + instance_capacity_;
    containers_ = storage.containers.data();
    scratch_containers_ = containers_ + container_capacity_ + 1U;
    draw_node_order_ = storage.draw_order.data();
    scratch_draw_node_order_ = draw_node_order_ + capacity_;
    node_changes_ = storage.node_changes.data();
    instance_changes_ = storage.instance_changes.data();
    container_changes_ = storage.container_changes.data();
    node_marks_ = storage.node_marks.data();
    instance_marks_ = storage.instance_marks.data();
    container_marks_ = storage.container_marks.data();
    text_capacity_ = static_cast<uint32_t>(storage.text.size() / 2U);
    text_arena_ = text;
    text_ = text;
    text_used_ = used;
}

uint32_t GuestScene::TextLive() const {
    uint32_t live = 0U;
    for (uint16_t index = 0U; index < node_count_; ++index) {
        if (current_[index].kind == GuestSceneNodeKind::kText) {
            live += static_cast<uint32_t>(current_[index].text_length) + 1U;
        }
    }
    return live;
}

void GuestScene::CompactText() {
    // The arena has two halves; live text moves to the other half so pointers
    // retained from the last committed scene stay readable until the next
    // compaction, which cannot happen before a new scene replaced them.
    if (text_arena_ == nullptr) {
        return;
    }
    char* destination = text_ == text_arena_ ? text_arena_ + text_capacity_ : text_arena_;
    uint32_t used = 0U;
    for (uint16_t index = 0U; index < node_count_; ++index) {
        GuestSceneNode& node = current_[index];
        if (node.kind != GuestSceneNodeKind::kText) {
            continue;
        }
        std::memcpy(destination + used, text_ + node.text_offset, static_cast<size_t>(node.text_length) + 1U);
        node.text_offset = used;
        used += static_cast<uint32_t>(node.text_length) + 1U;
    }
    text_ = destination;
    text_used_ = used;
}

int32_t GuestScene::Apply(const uint8_t* bytes, uint32_t length, int32_t logical_width, int32_t logical_height,
                          device::BitmapResolver bitmap_resolver, void* bitmap_context,
                          device::FontValidator font_validator, void* font_context) {
    micropixel_graphics_scene_header_t header{};
    if (length > micropixel::device::graphics_limits::kMaxSceneBytes || capacity_ == 0U || instance_capacity_ == 0U ||
        current_ == nullptr || scratch_ == nullptr || current_instances_ == nullptr || scratch_instances_ == nullptr ||
        containers_ == nullptr || scratch_containers_ == nullptr || draw_node_order_ == nullptr ||
        scratch_draw_node_order_ == nullptr || text_ == nullptr || node_marks_ == nullptr ||
        instance_marks_ == nullptr || container_marks_ == nullptr || logical_width <= 0 || logical_height <= 0 ||
        !Read(bytes, length, 0U, header) || header.magic != MICROPIXEL_GRAPHICS_SCENE_MAGIC || header.flags != 0U ||
        header.total_size != length || header.node_count > capacity_ ||
        header.batch_instance_count > instance_capacity_ || header.container_count > container_capacity_) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const bool keyframe = header.kind == MICROPIXEL_GRAPHICS_SCENE_KEYFRAME;
    const bool patch = header.kind == MICROPIXEL_GRAPHICS_SCENE_PATCH;
    if ((!keyframe && !patch) ||
        (keyframe && (header.generation == 0U || header.base_revision != 0U || header.revision != 1U)) ||
        (patch &&
         (!valid_ || revision_ == UINT32_MAX || header.generation != generation_ || header.base_revision != revision_ ||
          header.revision != revision_ + 1U || header.node_count != node_count_ ||
          header.container_count != container_count_ || header.batch_instance_count != batch_instance_count_))) {
        return patch ? MICROPIXEL_STATUS_STALE_STATE : MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }

    std::memset(node_changes_, 0, capacity_ * sizeof(*node_changes_));
    std::memset(container_changes_, 0, (static_cast<size_t>(container_capacity_) + 1U) * sizeof(*container_changes_));
    std::memset(instance_changes_, 0, instance_capacity_ * sizeof(*instance_changes_));
    std::memset(node_marks_, 0, header.node_count);
    std::memset(instance_marks_, 0, header.batch_instance_count);
    std::memset(container_marks_, 0, static_cast<size_t>(header.container_count) + 1U);
    std::memset(scratch_draw_node_order_, 0, static_cast<size_t>(capacity_) * sizeof(scratch_draw_node_order_[0]));
    last_apply_was_keyframe_ = false;
    background_changed_ = false;
    tree_order_changed_ = false;
    tree_order_changed_ = keyframe;

    uint16_t scratch_node_count = header.node_count;
    uint16_t scratch_container_count = header.container_count;
    uint16_t scratch_batch_instance_count = header.batch_instance_count;
    uint32_t scratch_background = background_rgb888_;
    if (keyframe) {
        for (uint16_t index = 0U; index < capacity_; ++index) {
            scratch_[index] = {};
        }
        for (uint16_t index = 0U; index <= scratch_container_count; ++index) {
            scratch_containers_[index] = {};
        }
        for (uint16_t index = 0U; index < instance_capacity_; ++index) {
            scratch_instances_[index] = {};
        }
        scratch_background = 0U;
    } else {
        for (uint16_t index = 0U; index < node_count_; ++index) {
            scratch_[index] = current_[index];
        }
        for (uint16_t index = 0U; index <= container_count_; ++index) {
            scratch_containers_[index] = containers_[index];
        }
        for (uint16_t index = 0U; index < batch_instance_count_; ++index) {
            scratch_instances_[index] = current_instances_[index];
        }
    }

    // Per-Apply marks live in storage: bit 0 = record seen, bit 1 = link seen.
    constexpr uint8_t kSeen = 0x01U;
    constexpr uint8_t kLinkSeen = 0x02U;
    bool background_seen = false;
    uint16_t next_instance_offset = 0U;
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
        } else if (record.opcode == MICROPIXEL_GRAPHICS_SCENE_OP_CONTAINER) {
            micropixel_graphics_scene_container_record_t value{};
            if (record.size != sizeof(value) || !Read(bytes, length, offset, value) || value.container_id == 0U ||
                value.container_id > scratch_container_count || value.parent_container_id > scratch_container_count ||
                value.parent_container_id == value.container_id || (value.flags & ~kKnownContainerFlags) != 0U ||
                (container_marks_[value.container_id] & kSeen) != 0U || value.property_mask == 0U ||
                (value.property_mask & ~kContainerMask) != 0U || (keyframe && value.property_mask != kContainerMask)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            GuestSceneContainer& container = scratch_containers_[value.container_id];
            if ((value.property_mask & MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAGS) != 0U) {
                container.cached_content =
                    (value.flags & MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAG_CACHED_CONTENT) != 0U;
            } else if (((value.flags & MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAG_CACHED_CONTENT) != 0U) !=
                       container.cached_content) {
                // Like sibling_order, a patch that does not claim the property
                // must echo the retained value.
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            if ((value.property_mask & MICROPIXEL_GRAPHICS_SCENE_CONTAINER_STRUCTURE) != 0U) {
                container.parent_container_id = value.parent_container_id;
                container.sibling_order = value.sibling_order;
            } else if (value.parent_container_id != container.parent_container_id ||
                       value.sibling_order != container.sibling_order) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            if ((value.property_mask & MICROPIXEL_GRAPHICS_SCENE_CONTAINER_CLIP) != 0U) {
                container.clip_x = value.clip_x;
                container.clip_y = value.clip_y;
                container.width = value.clip_width;
                container.height = value.clip_height;
            }
            if ((value.property_mask & MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION) != 0U) {
                container.translate_x = value.translate_x;
                container.translate_y = value.translate_y;
            }
            if ((value.property_mask & MICROPIXEL_GRAPHICS_SCENE_CONTAINER_APPEARANCE) != 0U) {
                if (value.visible > 1U) {
                    return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                }
                container.opacity = value.opacity;
                container.visible = value.visible != 0U;
            }
            if ((value.property_mask & MICROPIXEL_GRAPHICS_SCENE_CONTAINER_Z_ORDER) != 0U) {
                container.z_order = value.z_order;
            }
            container_marks_[value.container_id] = kSeen;
            container_changes_[value.container_id] = static_cast<uint8_t>(value.property_mask);
            tree_order_changed_ =
                tree_order_changed_ || (value.property_mask & (MICROPIXEL_GRAPHICS_SCENE_CONTAINER_Z_ORDER |
                                                               MICROPIXEL_GRAPHICS_SCENE_CONTAINER_STRUCTURE)) != 0U;
        } else if (record.opcode == MICROPIXEL_GRAPHICS_SCENE_OP_NODE_LINK) {
            micropixel_graphics_scene_node_link_record_t value{};
            if (!keyframe || record.size != sizeof(value) || !Read(bytes, length, offset, value) ||
                value.node_id >= scratch_node_count || value.parent_container_id > scratch_container_count ||
                value.reserved0 != 0U || node_marks_[value.node_id] != kSeen) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            scratch_[value.node_id].parent_container_id = value.parent_container_id;
            scratch_[value.node_id].sibling_order = value.sibling_order;
            node_marks_[value.node_id] |= kLinkSeen;
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
                if (scene_instance >= scratch_batch_instance_count || instance_marks_[scene_instance] != 0U ||
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
                instance_marks_[scene_instance] = kSeen;
                instance_changes_[scene_instance] = static_cast<uint8_t>(value.property_mask);
            }
        } else {
            micropixel_graphics_scene_node_header_t node_header{};
            const uint32_t required_mask = RequiredMask(record.opcode);
            if (required_mask == 0U || !Read(bytes, length, offset, node_header) ||
                node_header.node_id >= scratch_node_count || node_header.reserved0 != 0U ||
                node_marks_[node_header.node_id] != 0U || node_header.property_mask == 0U ||
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
            } else if (record.opcode == MICROPIXEL_GRAPHICS_SCENE_OP_ROUNDED_RECT) {
                micropixel_graphics_scene_rounded_rect_record_t value{};
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
                    node.radius = value.radius;
                    node.stroke_width = value.stroke_width;
                }
                if ((value.node.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE) != 0U) {
                    node.rgb888 = value.fill_rgb888;
                    node.stroke_rgb888 = value.stroke_rgb888;
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
                    node.texture_handle = value.texture_handle;
                    node.source_x = value.source_x;
                    node.source_y = value.source_y;
                    node.source_width = value.source_width;
                    node.source_height = value.source_height;
                }
            } else if (record.opcode == MICROPIXEL_GRAPHICS_SCENE_OP_SPRITE_BATCH) {
                micropixel_graphics_scene_sprite_batch_record_t value{};
                if (record.size != sizeof(value) || !Read(bytes, length, offset, value) || value.reserved0 != 0U ||
                    (value.node.flags & ~MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE) != 0U || value.capacity == 0U) {
                    return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                }
                if (keyframe) {
                    // Batches are bounded by the node budget and by the total
                    // instance count the header declared; no separate cap.
                    if (static_cast<uint32_t>(next_instance_offset) + value.capacity > scratch_batch_instance_count) {
                        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                    }
                    node.batch_instance_offset = next_instance_offset;
                    node.batch_capacity = value.capacity;
                    next_instance_offset += value.capacity;
                } else if (value.capacity != node.batch_capacity) {
                    return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                }
                if ((value.node.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE) != 0U) {
                    node.opacity = value.opacity;
                }
                if ((value.node.property_mask & MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT) != 0U) {
                    node.texture_handle = value.texture_handle;
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
                    value.text_length > micropixel::device::graphics_limits::kMaxTextBytes || value.reserved0 != 0U ||
                    record.size != expected_size ||
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
                    // The owner sized the arena for this message; failing here
                    // means the storage contract was broken, not the Guest.
                    if (text_capacity_ - text_used_ < static_cast<uint32_t>(value.text_length) + 1U) {
                        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
                    }
                    node.font_handle = value.font_handle;
                    node.text_length = value.text_length;
                    node.text_offset = text_used_;
                    std::memcpy(text_ + text_used_, bytes + offset + sizeof(value), value.text_length);
                    text_[text_used_ + value.text_length] = '\0';
                    text_used_ += static_cast<uint32_t>(value.text_length) + 1U;
                }
            }
            node_marks_[node_header.node_id] |= kSeen;
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
            if (node_marks_[node] != (kSeen | kLinkSeen)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
        }
        for (uint16_t container = 1U; container <= scratch_container_count; ++container) {
            if (container_marks_[container] == 0U) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
        }
        for (uint16_t instance = 0U; instance < scratch_batch_instance_count; ++instance) {
            if (instance_marks_[instance] == 0U) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
        }
    }
    if (!ValidateResult(bitmap_resolver, bitmap_context, font_validator, font_context, scratch_node_count,
                        scratch_container_count, scratch_batch_instance_count)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }

    std::swap(current_, scratch_);
    std::swap(current_instances_, scratch_instances_);
    std::swap(containers_, scratch_containers_);
    std::swap(draw_node_order_, scratch_draw_node_order_);
    node_count_ = scratch_node_count;
    container_count_ = scratch_container_count;
    batch_instance_count_ = scratch_batch_instance_count;
    background_rgb888_ = scratch_background;
    generation_ = header.generation;
    revision_ = header.revision;
    last_apply_was_keyframe_ = keyframe;
    valid_ = true;
    return MICROPIXEL_STATUS_OK;
}

bool GuestScene::ValidateResult(device::BitmapResolver bitmap_resolver, void* bitmap_context,
                                device::FontValidator font_validator, void* font_context, uint16_t node_count,
                                uint16_t container_count, uint16_t batch_instance_count) {
    for (uint16_t index = 1U; index <= container_count; ++index) {
        const GuestSceneContainer& container = scratch_containers_[index];
        const bool inherited_clip =
            container.clip_x == 0 && container.clip_y == 0 && container.width == 0 && container.height == 0;
        if (container.parent_container_id > container_count || container.parent_container_id == index ||
            (!inherited_clip &&
             !ValidClippedLocalRect(container.clip_x, container.clip_y, container.width, container.height))) {
            return false;
        }
        uint16_t ancestor = container.parent_container_id;
        for (uint16_t depth = 0U; ancestor != 0U && depth <= container_count; ++depth) {
            ancestor = scratch_containers_[ancestor].parent_container_id;
            if (depth == container_count) {
                return false;
            }
        }
    }
    for (uint16_t index = 0U; index < node_count; ++index) {
        GuestSceneNode& node = scratch_[index];
        if (node.parent_container_id > container_count) {
            return false;
        }
        const auto valid_geometry = [&](int32_t x, int32_t y, int32_t width, int32_t height) {
            return ValidClippedLocalRect(x, y, width, height);
        };
        if (node.kind == GuestSceneNodeKind::kRect) {
            if (!valid_geometry(node.x, node.y, node.width, node.height) || !ValidRgb888(node.rgb888)) {
                return false;
            }
        } else if (node.kind == GuestSceneNodeKind::kRoundedRect) {
            if (!valid_geometry(node.x, node.y, node.width, node.height) || !ValidRgb888(node.rgb888) ||
                !ValidRgb888(node.stroke_rgb888)) {
                return false;
            }
        } else if (node.kind == GuestSceneNodeKind::kTexture) {
            device::BitmapView bitmap{};
            if (!valid_geometry(node.x, node.y, node.width, node.height) || node.texture_handle == 0U ||
                bitmap_resolver == nullptr || !bitmap_resolver(bitmap_context, node.texture_handle, bitmap) ||
                !ValidBitmap(bitmap)) {
                return false;
            }
            const int32_t original_source_width = node.source_width;
            const int32_t original_source_height = node.source_height;
            if (!NormalizeRoundedTextureExtent(node.source_x, node.source_width, bitmap.width) ||
                !NormalizeRoundedTextureExtent(node.source_y, node.source_height, bitmap.height)) {
                return false;
            }
            if (node.source_width != original_source_width || node.source_height != original_source_height) {
                node_changes_[index] |= MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT;
            }
        } else if (node.kind == GuestSceneNodeKind::kSpriteBatch) {
            device::BitmapView bitmap{};
            const bool textured = node.texture_handle != 0U;
            if (node.batch_capacity == 0U ||
                static_cast<uint32_t>(node.batch_instance_offset) + node.batch_capacity > batch_instance_count ||
                (textured && (bitmap_resolver == nullptr ||
                              !bitmap_resolver(bitmap_context, node.texture_handle, bitmap) || !ValidBitmap(bitmap)))) {
                return false;
            }
            for (uint16_t instance_index = 0U; instance_index < node.batch_capacity; ++instance_index) {
                GuestSceneSpriteInstance& instance = scratch_instances_[node.batch_instance_offset + instance_index];
                if ((instance.flags & MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBLE) == 0U) {
                    continue;
                }
                if (!valid_geometry(instance.x, instance.y, instance.width, instance.height) ||
                    !ValidRgb888(instance.rgb888)) {
                    return false;
                }
                if (textured) {
                    const int32_t original_source_width = instance.source_width;
                    const int32_t original_source_height = instance.source_height;
                    if (!NormalizeRoundedTextureExtent(instance.source_x, instance.source_width, bitmap.width) ||
                        !NormalizeRoundedTextureExtent(instance.source_y, instance.source_height, bitmap.height)) {
                        return false;
                    }
                    if (instance.source_width != original_source_width ||
                        instance.source_height != original_source_height) {
                        instance_changes_[node.batch_instance_offset + instance_index] |=
                            MICROPIXEL_GRAPHICS_SCENE_INSTANCE_CONTENT;
                    }
                }
            }
        } else if (!ValidRgb888(node.rgb888) || node.font_handle == 0U || node.text_length == 0U ||
                   node.text_length > micropixel::device::graphics_limits::kMaxTextBytes || font_validator == nullptr ||
                   !font_validator(font_context, node.font_handle) ||
                   node.text_offset + node.text_length >= text_capacity_ ||
                   !device::IsValidUtf8(reinterpret_cast<const uint8_t*>(text_ + node.text_offset), node.text_length)) {
            return false;
        }
    }
    for (uint16_t container = 1U; container <= container_count; ++container) {
        for (uint16_t other = static_cast<uint16_t>(container + 1U); other <= container_count; ++other) {
            if (scratch_containers_[container].parent_container_id == scratch_containers_[other].parent_container_id &&
                scratch_containers_[container].sibling_order == scratch_containers_[other].sibling_order) {
                return false;
            }
        }
        for (uint16_t node = 0U; node < node_count; ++node) {
            if (scratch_containers_[container].parent_container_id == scratch_[node].parent_container_id &&
                scratch_containers_[container].sibling_order == scratch_[node].sibling_order) {
                return false;
            }
        }
    }
    for (uint16_t node = 0U; node < node_count; ++node) {
        for (uint16_t other = static_cast<uint16_t>(node + 1U); other < node_count; ++other) {
            if (scratch_[node].parent_container_id == scratch_[other].parent_container_id &&
                scratch_[node].sibling_order == scratch_[other].sibling_order) {
                return false;
            }
        }
    }
    return BuildDrawOrder(node_count, container_count);
}

bool GuestScene::BuildDrawOrder(uint16_t node_count, uint16_t container_count) {
    uint16_t output_count = 0U;
    return AppendChildren(0U, node_count, container_count, output_count) && output_count == node_count;
}

bool GuestScene::AppendChildren(uint16_t parent_id, uint16_t node_count, uint16_t container_count,
                                uint16_t& output_count) {
    bool have_last = false;
    int16_t last_z = INT16_MIN;
    uint16_t last_sibling = 0U;
    while (true) {
        bool found = false;
        bool selected_container = false;
        uint16_t selected_id = 0U;
        int16_t selected_z = INT16_MAX;
        uint16_t selected_sibling = UINT16_MAX;
        for (uint16_t container_id = 1U; container_id <= container_count; ++container_id) {
            const GuestSceneContainer& container = scratch_containers_[container_id];
            if (container.parent_container_id != parent_id ||
                (have_last && (container.z_order < last_z ||
                               (container.z_order == last_z && container.sibling_order <= last_sibling)))) {
                continue;
            }
            if (!found || container.z_order < selected_z ||
                (container.z_order == selected_z && container.sibling_order < selected_sibling)) {
                found = true;
                selected_container = true;
                selected_id = container_id;
                selected_z = container.z_order;
                selected_sibling = container.sibling_order;
            }
        }
        for (uint16_t node_id = 0U; node_id < node_count; ++node_id) {
            const GuestSceneNode& node = scratch_[node_id];
            constexpr int16_t node_z = 0;
            if (node.parent_container_id != parent_id ||
                (have_last && (node_z < last_z || (node_z == last_z && node.sibling_order <= last_sibling)))) {
                continue;
            }
            if (!found || node_z < selected_z || (node_z == selected_z && node.sibling_order < selected_sibling)) {
                found = true;
                selected_container = false;
                selected_id = node_id;
                selected_z = node_z;
                selected_sibling = node.sibling_order;
            }
        }
        if (!found) {
            return true;
        }
        last_z = selected_z;
        last_sibling = selected_sibling;
        have_last = true;
        if (selected_container) {
            if (!AppendChildren(selected_id, node_count, container_count, output_count)) {
                return false;
            }
        } else {
            if (output_count >= node_count) {
                return false;
            }
            scratch_draw_node_order_[output_count++] = selected_id;
        }
    }
}

uint32_t GuestScene::AncestorChanges(uint16_t container_id) const {
    uint32_t changes = 0U;
    for (uint16_t depth = 0U; container_id != 0U && depth < container_count_; ++depth) {
        changes |= container_changes_[container_id];
        container_id = containers_[container_id].parent_container_id;
    }
    return changes;
}

void GuestScene::Reset() {
    node_count_ = 0U;
    container_count_ = 0U;
    batch_instance_count_ = 0U;
    background_rgb888_ = 0U;
    generation_ = 0U;
    revision_ = 0U;
    std::memset(node_changes_, 0, capacity_ * sizeof(*node_changes_));
    if (container_changes_ != nullptr) {
        std::memset(container_changes_, 0,
                    (static_cast<size_t>(container_capacity_) + 1U) * sizeof(*container_changes_));
    }
    std::memset(instance_changes_, 0, instance_capacity_ * sizeof(*instance_changes_));
    text_used_ = 0U;
    if (draw_node_order_ != nullptr && scratch_draw_node_order_ != nullptr) {
        std::memset(draw_node_order_, 0, static_cast<size_t>(capacity_) * sizeof(draw_node_order_[0]));
        std::memset(scratch_draw_node_order_, 0, static_cast<size_t>(capacity_) * sizeof(scratch_draw_node_order_[0]));
    }
    last_apply_was_keyframe_ = false;
    background_changed_ = false;
    tree_order_changed_ = false;
    valid_ = false;
    if (containers_ != nullptr && scratch_containers_ != nullptr) {
        for (uint16_t index = 0U; index <= container_capacity_; ++index) {
            containers_[index] = {};
            scratch_containers_[index] = {};
        }
    }
}

}  // namespace micropixel::platform::graphics
