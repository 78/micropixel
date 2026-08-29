#include "platform/graphics/app_surface_compositor.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "abi/micropixel_abi.h"

namespace micropixel::platform::graphics {
namespace {

uint32_t BytesPerPixel(SurfacePixelFormat format) {
    switch (format) {
        case SurfacePixelFormat::kBgr888:
        case SurfacePixelFormat::kBgra8888:
            return format == SurfacePixelFormat::kBgr888 ? 3U : 4U;
        case SurfacePixelFormat::kRgb565:
            return 2U;
    }
    return 0U;
}

bool ValidDestination(PixelSurface surface) {
    const uint32_t bytes_per_pixel = BytesPerPixel(surface.format);
    if (surface.pixels == nullptr || surface.width == 0U || surface.height == 0U || bytes_per_pixel == 0U ||
        surface.format == SurfacePixelFormat::kBgra8888 || surface.width > UINT32_MAX / bytes_per_pixel) {
        return false;
    }
    const uint32_t row_bytes = surface.width * bytes_per_pixel;
    const uint64_t required = static_cast<uint64_t>(surface.stride) * (surface.height - 1U) + row_bytes;
    return surface.stride >= row_bytes && required <= surface.size;
}

bool BitmapSurface(const device::BitmapView& bitmap, ConstPixelSurface& surface) {
    SurfacePixelFormat format{};
    uint32_t bytes_per_pixel = 0U;
    if (bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888) {
        format = SurfacePixelFormat::kBgr888;
        bytes_per_pixel = 3U;
    } else if (bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888) {
        format = SurfacePixelFormat::kBgra8888;
        bytes_per_pixel = 4U;
    } else {
        return false;
    }
    if (bitmap.data == nullptr || bitmap.width == 0U || bitmap.height == 0U ||
        bitmap.width > UINT32_MAX / bytes_per_pixel) {
        return false;
    }
    const uint32_t row_bytes = bitmap.width * bytes_per_pixel;
    const uint64_t required = static_cast<uint64_t>(bitmap.stride) * (bitmap.height - 1U) + row_bytes;
    if (bitmap.stride < row_bytes || required > bitmap.size) {
        return false;
    }
    surface = {
        .pixels = bitmap.data,
        .size = bitmap.size,
        .width = bitmap.width,
        .height = bitmap.height,
        .stride = bitmap.stride,
        .format = format,
    };
    return true;
}

bool SameRect(const SurfaceRect& left, const SurfaceRect& right) {
    return left.x == right.x && left.y == right.y && left.width == right.width && left.height == right.height;
}

bool SameBitmap(const device::BitmapView& left, const device::BitmapView& right) {
    return left.data == right.data && left.size == right.size && left.width == right.width &&
           left.height == right.height && left.stride == right.stride && left.pixel_format == right.pixel_format &&
           left.flags == right.flags;
}

bool SameVisual(const AppDrawOperation& left, const AppDrawOperation& right) {
    if (!left.visible && !right.visible) {
        return true;
    }
    if (left.visible != right.visible || left.in_layer != right.in_layer || left.kind != right.kind ||
        left.z_order != right.z_order || left.stable_order != right.stable_order ||
        !SameRect(left.destination, right.destination) || !SameRect(left.bounds, right.bounds) ||
        left.opacity != right.opacity) {
        return false;
    }
    if (left.kind == AppDrawOperationKind::kFill) {
        return left.rgb888 == right.rgb888;
    }
    if (left.kind == AppDrawOperationKind::kTexture) {
        return left.texture == right.texture && SameRect(left.source, right.source) &&
               SameBitmap(left.bitmap, right.bitmap);
    }
    return left.rgb888 == right.rgb888 && left.font == right.font && left.text_length == right.text_length &&
           std::memcmp(left.text, right.text, left.text_length) == 0;
}

bool Intersects(SurfaceRect left, SurfaceRect right) {
    const int64_t left_right = static_cast<int64_t>(left.x) + left.width;
    const int64_t left_bottom = static_cast<int64_t>(left.y) + left.height;
    const int64_t right_right = static_cast<int64_t>(right.x) + right.width;
    const int64_t right_bottom = static_cast<int64_t>(right.y) + right.height;
    return left.x < right_right && right.x < left_right && left.y < right_bottom && right.y < left_bottom;
}

SurfaceRect Intersection(SurfaceRect left, SurfaceRect right) {
    const int64_t left_edge = left.x > right.x ? left.x : right.x;
    const int64_t top_edge = left.y > right.y ? left.y : right.y;
    const int64_t left_right = static_cast<int64_t>(left.x) + left.width;
    const int64_t right_right = static_cast<int64_t>(right.x) + right.width;
    const int64_t left_bottom = static_cast<int64_t>(left.y) + left.height;
    const int64_t right_bottom = static_cast<int64_t>(right.y) + right.height;
    const int64_t right_edge = left_right < right_right ? left_right : right_right;
    const int64_t bottom_edge = left_bottom < right_bottom ? left_bottom : right_bottom;
    if (right_edge <= left_edge || bottom_edge <= top_edge) {
        return {};
    }
    return {.x = static_cast<int32_t>(left_edge),
            .y = static_cast<int32_t>(top_edge),
            .width = static_cast<int32_t>(right_edge - left_edge),
            .height = static_cast<int32_t>(bottom_edge - top_edge)};
}

SurfaceRect Translate(SurfaceRect rect, int32_t x, int32_t y) {
    rect.x -= x;
    rect.y -= y;
    return rect;
}

bool CropBlit(const AppDrawOperation& operation, SurfaceRect draw_bounds, SurfaceRect& source,
              SurfaceRect& destination) {
    if (operation.destination.width <= 0 || operation.destination.height <= 0 || operation.source.width <= 0 ||
        operation.source.height <= 0) {
        return false;
    }
    const int64_t relative_left = static_cast<int64_t>(draw_bounds.x) - operation.destination.x;
    const int64_t relative_top = static_cast<int64_t>(draw_bounds.y) - operation.destination.y;
    const int64_t relative_right = relative_left + draw_bounds.width;
    const int64_t relative_bottom = relative_top + draw_bounds.height;
    if (relative_left < 0 || relative_top < 0 || relative_right > operation.destination.width ||
        relative_bottom > operation.destination.height) {
        return false;
    }
    const int64_t source_left = relative_left * operation.source.width;
    const int64_t source_top = relative_top * operation.source.height;
    const int64_t source_right = relative_right * operation.source.width;
    const int64_t source_bottom = relative_bottom * operation.source.height;
    if (source_left % operation.destination.width != 0 || source_right % operation.destination.width != 0 ||
        source_top % operation.destination.height != 0 || source_bottom % operation.destination.height != 0) {
        return false;
    }
    source = {
        .x = static_cast<int32_t>(operation.source.x + source_left / operation.destination.width),
        .y = static_cast<int32_t>(operation.source.y + source_top / operation.destination.height),
        .width = static_cast<int32_t>((source_right - source_left) / operation.destination.width),
        .height = static_cast<int32_t>((source_bottom - source_top) / operation.destination.height),
    };
    destination = {.x = 0, .y = 0, .width = draw_bounds.width, .height = draw_bounds.height};
    return source.width > 0 && source.height > 0;
}

bool Offset(SurfaceRect rect, int32_t x, int32_t y, SurfaceRect& result) {
    const int64_t translated_x = static_cast<int64_t>(rect.x) + x;
    const int64_t translated_y = static_cast<int64_t>(rect.y) + y;
    if (translated_x < INT32_MIN || translated_x > INT32_MAX || translated_y < INT32_MIN || translated_y > INT32_MAX) {
        return false;
    }
    result = rect;
    result.x = static_cast<int32_t>(translated_x);
    result.y = static_cast<int32_t>(translated_y);
    return true;
}

bool SameLayerDefinition(const AppLayerState& left, const AppLayerState& right) {
    return left.valid == right.valid &&
           (!left.valid || (left.layer_id == right.layer_id && SameRect(left.clip, right.clip)));
}

bool SameLayerVisual(const AppDrawOperation& current, const AppLayerState& current_layer, const AppDrawOperation& next,
                     const AppLayerState& next_layer) {
    AppDrawOperation current_local = current;
    current_local.destination.x -= current_layer.translate_x;
    current_local.destination.y -= current_layer.translate_y;
    current_local.bounds.x -= current_layer.translate_x;
    current_local.bounds.y -= current_layer.translate_y;
    AppDrawOperation next_local = next;
    next_local.destination.x -= next_layer.translate_x;
    next_local.destination.y -= next_layer.translate_y;
    next_local.bounds.x -= next_layer.translate_x;
    next_local.bounds.y -= next_layer.translate_y;
    return SameVisual(current_local, next_local);
}

bool LayerContentChanged(const AppDrawOperation* current, uint32_t current_count, const AppLayerState& current_layer,
                         const AppDrawOperation* next, uint32_t next_count, const AppLayerState& next_layer) {
    const uint32_t compared_count = current_count > next_count ? current_count : next_count;
    for (uint32_t index = 0U; index < compared_count; ++index) {
        const bool old_exists = index < current_count;
        const bool new_exists = index < next_count;
        const bool touches_layer = (old_exists && current[index].in_layer) || (new_exists && next[index].in_layer);
        if (touches_layer && (!old_exists || !new_exists || current[index].stable_order != next[index].stable_order ||
                              !SameLayerVisual(current[index], current_layer, next[index], next_layer))) {
            return true;
        }
    }
    return false;
}

bool LayerRect(const AppLayerState& layer, SurfaceRect& rect) {
    return layer.valid && Offset(layer.clip, layer.translate_x, layer.translate_y, rect);
}

bool FullSurfaceRect(PixelSurface destination, SurfaceRect& rect) {
    if (destination.width > INT32_MAX || destination.height > INT32_MAX) {
        return false;
    }
    rect = {.x = 0,
            .y = 0,
            .width = static_cast<int32_t>(destination.width),
            .height = static_cast<int32_t>(destination.height)};
    return true;
}

PixelSurface SubSurface(PixelSurface destination, DamageRect damage) {
    return {
        .pixels = destination.pixels,
        .size = destination.size,
        .width = damage.width,
        .height = damage.height,
        .stride = destination.stride,
        .format = destination.format,
        .origin_x = destination.origin_x + damage.x,
        .origin_y = destination.origin_y + damage.y,
    };
}

uint64_t DamagePixels(const DamageRegionSet<AppSurfaceCompositor::kMaxDamageRegions>& damage) {
    uint64_t pixels = 0U;
    for (size_t index = 0U; index < damage.Size(); ++index) {
        pixels += static_cast<uint64_t>(damage[index].rect.width) * damage[index].rect.height;
    }
    return pixels;
}

AppLayerState NormalizeLayer(const GuestScene& scene) {
    if (scene.LayerCount() < 1U) {
        return {};
    }
    const GuestSceneLayer& source = scene.Layers()[1];
    return {
        .valid = source.visible && source.opacity == 255U,
        .translation_active = source.translate_x != 0 || source.translate_y != 0,
        .layer_id = 1U,
        .clip = {.x = source.clip_x, .y = source.clip_y, .width = source.width, .height = source.height},
        .translate_x = source.translate_x,
        .translate_y = source.translate_y,
    };
}

}  // namespace

AppSurfaceStatus AppSurfaceCompositor::NormalizeOperation(const GuestScene& scene, const GuestSceneNode& node,
                                                          const GuestSceneSpriteInstance* instance,
                                                          uint16_t stable_order, const AppLayerState& layer,
                                                          device::BitmapResolver resolver, void* resolver_context,
                                                          AppDrawOperation& operation) const {
    operation = {};
    operation.stable_order = stable_order;
    if (node.kind == GuestSceneNodeKind::kSpriteBatch) {
        if (instance == nullptr) {
            return AppSurfaceStatus::kInvalidArgument;
        }
        const bool textured = node.texture != 0U;
        ConstPixelSurface bitmap_surface{};
        if (textured && (resolver == nullptr || !resolver(resolver_context, node.texture, operation.bitmap) ||
                         !BitmapSurface(operation.bitmap, bitmap_surface))) {
            return AppSurfaceStatus::kUnsupported;
        }
        operation.kind = textured ? AppDrawOperationKind::kTexture : AppDrawOperationKind::kFill;
        operation.visible = node.visible && (instance->flags & MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBLE) != 0U;
        operation.opacity =
            static_cast<uint8_t>((static_cast<uint32_t>(node.opacity) * instance->opacity + 127U) / 255U);
        operation.texture = node.texture;
        operation.rgb888 = instance->rgb888;
        operation.destination = {
            .x = instance->x, .y = instance->y, .width = instance->width, .height = instance->height};
        operation.source = {.x = instance->source_x,
                            .y = instance->source_y,
                            .width = instance->source_width,
                            .height = instance->source_height};
    } else {
        if (instance != nullptr) {
            return AppSurfaceStatus::kInvalidArgument;
        }
        operation.visible = node.visible;
        operation.opacity = node.opacity;
        operation.rgb888 = node.rgb888;
        operation.destination = {.x = node.x, .y = node.y, .width = node.width, .height = node.height};
        if (node.kind == GuestSceneNodeKind::kRect) {
            operation.kind = AppDrawOperationKind::kFill;
        } else if (node.kind == GuestSceneNodeKind::kTexture) {
            if (resolver == nullptr || !resolver(resolver_context, node.texture, operation.bitmap)) {
                return AppSurfaceStatus::kInvalidArgument;
            }
            ConstPixelSurface bitmap_surface{};
            if (!BitmapSurface(operation.bitmap, bitmap_surface)) {
                return AppSurfaceStatus::kUnsupported;
            }
            operation.kind = AppDrawOperationKind::kTexture;
            operation.texture = node.texture;
            operation.source = {
                .x = node.source_x, .y = node.source_y, .width = node.source_width, .height = node.source_height};
        } else {
            if (text_ == nullptr) {
                return AppSurfaceStatus::kUnsupported;
            }
            operation.kind = AppDrawOperationKind::kText;
            operation.opacity = 255U;
            operation.font = node.font;
            operation.text_length = node.text_length;
            std::memcpy(operation.text, node.text, node.text_length + 1U);
            RasterTextMetrics metrics{};
            if (!text_->Measure(operation.font, operation.text, operation.text_length, metrics) ||
                metrics.width > INT32_MAX || metrics.height > INT32_MAX) {
                return AppSurfaceStatus::kInvalidArgument;
            }
            operation.destination.x = node.text_centered ? node.x - static_cast<int32_t>(metrics.width) / 2 : node.x;
            operation.destination.width = static_cast<int32_t>(metrics.width);
            operation.destination.height = static_cast<int32_t>(metrics.height);
        }
    }

    operation.bounds = operation.destination;
    if (node.layer_id != 0U) {
        const GuestSceneLayer& source_layer = scene.Layers()[node.layer_id];
        SurfaceRect translated_destination{};
        SurfaceRect translated_clip{};
        const SurfaceRect clip{.x = source_layer.clip_x,
                               .y = source_layer.clip_y,
                               .width = source_layer.width,
                               .height = source_layer.height};
        if (!Offset(operation.destination, source_layer.translate_x, source_layer.translate_y,
                    translated_destination) ||
            !Offset(clip, source_layer.translate_x, source_layer.translate_y, translated_clip)) {
            return AppSurfaceStatus::kInvalidArgument;
        }
        operation.in_layer = node.layer_id == layer.layer_id;
        operation.z_order = source_layer.z_order;
        operation.visible = operation.visible && source_layer.visible;
        operation.opacity =
            static_cast<uint8_t>((static_cast<uint32_t>(operation.opacity) * source_layer.opacity + 127U) / 255U);
        operation.destination = translated_destination;
        operation.bounds = Intersection(translated_destination, translated_clip);
    }
    operation.visible =
        operation.visible && operation.opacity != 0U && operation.bounds.width > 0 && operation.bounds.height > 0;
    return AppSurfaceStatus::kOk;
}

void AppSurfaceCompositor::SortOperations(AppDrawOperation* operations, uint32_t operation_count) {
    for (uint32_t index = 1U; index < operation_count; ++index) {
        AppDrawOperation value = operations[index];
        uint32_t insertion = index;
        while (insertion > 0U && (operations[insertion - 1U].z_order > value.z_order ||
                                  (operations[insertion - 1U].z_order == value.z_order &&
                                   operations[insertion - 1U].stable_order > value.stable_order))) {
            operations[insertion] = operations[insertion - 1U];
            --insertion;
        }
        operations[insertion] = value;
    }
}

AppSurfaceStatus AppSurfaceCompositor::NormalizeScene(const GuestScene& scene, device::BitmapResolver resolver,
                                                      void* resolver_context, uint32_t& operation_count,
                                                      AppLayerState& layer) {
    if (current_ == nullptr || scratch_ == nullptr) {
        return AppSurfaceStatus::kResourceExhausted;
    }
    operation_count = 0U;
    layer = NormalizeLayer(scene);
    uint16_t stable_order = 0U;
    for (uint32_t index = 0U; index < scene.NodeCount(); ++index) {
        const GuestSceneNode& node = scene.Nodes()[index];
        if (node.kind == GuestSceneNodeKind::kSpriteBatch) {
            for (uint16_t instance_index = 0U; instance_index < node.batch_capacity; ++instance_index) {
                if (operation_count >= operation_capacity_) {
                    return AppSurfaceStatus::kResourceExhausted;
                }
                const GuestSceneSpriteInstance& instance =
                    scene.Instances()[node.batch_instance_offset + instance_index];
                AppDrawOperation operation{};
                const AppSurfaceStatus status = NormalizeOperation(scene, node, &instance, stable_order++, layer,
                                                                   resolver, resolver_context, operation);
                if (status != AppSurfaceStatus::kOk) {
                    return status;
                }
                scratch_[operation_count++] = operation;
            }
            continue;
        }
        if (operation_count >= operation_capacity_) {
            return AppSurfaceStatus::kResourceExhausted;
        }
        AppDrawOperation operation{};
        const AppSurfaceStatus status =
            NormalizeOperation(scene, node, nullptr, stable_order++, layer, resolver, resolver_context, operation);
        if (status != AppSurfaceStatus::kOk) {
            return status;
        }
        scratch_[operation_count++] = operation;
    }
    SortOperations(scratch_, operation_count);
    return AppSurfaceStatus::kOk;
}

AppSurfaceStatus AppSurfaceCompositor::NormalizeScenePatch(const GuestScene& scene, device::BitmapResolver resolver,
                                                           void* resolver_context, uint32_t& operation_count,
                                                           AppLayerState& layer) {
    if (current_ == nullptr || scratch_ == nullptr || current_count_ > operation_capacity_) {
        return AppSurfaceStatus::kResourceExhausted;
    }
    if (!scratch_synchronized_) {
        std::memcpy(scratch_, current_, current_count_ * sizeof(current_[0]));
    } else {
        for (uint16_t index = 0U; index < stale_operation_count_; ++index) {
            const uint16_t operation_index = stale_operation_indices_[index];
            scratch_[operation_index] = current_[operation_index];
        }
    }
    scratch_synchronized_ = true;
    stale_operation_count_ = 0U;
    operation_count = current_count_;
    layer = NormalizeLayer(scene);

    for (uint32_t index = 0U; index < current_count_; ++index) {
        const uint16_t stable_order = current_[index].stable_order;
        if (stable_order >= current_count_) {
            return AppSurfaceStatus::kInvalidArgument;
        }
        stable_to_sorted_index_[stable_order] = static_cast<uint16_t>(index);
    }

    bool z_order_changed = false;
    uint16_t stable_order = 0U;
    for (uint16_t node_index = 0U; node_index < scene.NodeCount(); ++node_index) {
        const GuestSceneNode& node = scene.Nodes()[node_index];
        const uint32_t node_changes = scene.NodeChanges(node_index);
        const uint32_t layer_changes = node.layer_id == 0U ? 0U : scene.LayerChanges(node.layer_id);
        z_order_changed = z_order_changed || (layer_changes & MICROPIXEL_GRAPHICS_SCENE_LAYER_Z_ORDER) != 0U;
        const bool node_or_layer_changed = node_changes != 0U || layer_changes != 0U;
        const uint16_t operation_span =
            node.kind == GuestSceneNodeKind::kSpriteBatch ? node.batch_capacity : static_cast<uint16_t>(1U);
        for (uint16_t local_index = 0U; local_index < operation_span; ++local_index) {
            if (stable_order >= current_count_) {
                return AppSurfaceStatus::kInvalidArgument;
            }
            const uint16_t scene_instance = node.batch_instance_offset + local_index;
            const bool changed = node_or_layer_changed || (node.kind == GuestSceneNodeKind::kSpriteBatch &&
                                                           scene.InstanceChanges(scene_instance) != 0U);
            if (changed) {
                ++normalized_operations_;
                AppDrawOperation operation{};
                const GuestSceneSpriteInstance* instance =
                    node.kind == GuestSceneNodeKind::kSpriteBatch ? &scene.Instances()[scene_instance] : nullptr;
                const AppSurfaceStatus status = NormalizeOperation(scene, node, instance, stable_order, layer, resolver,
                                                                   resolver_context, operation);
                if (status != AppSurfaceStatus::kOk) {
                    return status;
                }
                const uint16_t sorted_index = stable_to_sorted_index_[stable_order];
                if (!SameVisual(current_[sorted_index], operation)) {
                    stale_operation_indices_[stale_operation_count_++] = sorted_index;
                }
                scratch_[sorted_index] = operation;
            }
            ++stable_order;
        }
    }
    if (stable_order != current_count_) {
        return AppSurfaceStatus::kInvalidArgument;
    }
    if (z_order_changed) {
        SortOperations(scratch_, operation_count);
        stale_operation_count_ = static_cast<uint16_t>(operation_count);
        for (uint16_t index = 0U; index < stale_operation_count_; ++index) {
            stale_operation_indices_[index] = index;
        }
    }
    return AppSurfaceStatus::kOk;
}

bool AppSurfaceCompositor::AddDamage(PixelSurface destination, SurfaceRect rect) {
    if (rect.width <= 0 || rect.height <= 0) {
        return true;
    }
    const int64_t right = static_cast<int64_t>(rect.x) + rect.width;
    const int64_t bottom = static_cast<int64_t>(rect.y) + rect.height;
    const int64_t clipped_left = rect.x > 0 ? rect.x : 0;
    const int64_t clipped_top = rect.y > 0 ? rect.y : 0;
    const int64_t clipped_right = right < destination.width ? right : destination.width;
    const int64_t clipped_bottom = bottom < destination.height ? bottom : destination.height;
    if (clipped_right <= clipped_left || clipped_bottom <= clipped_top) {
        return true;
    }
    return damage_.Add(destination.pixels,
                       {.x = static_cast<uint32_t>(clipped_left),
                        .y = static_cast<uint32_t>(clipped_top),
                        .width = static_cast<uint32_t>(clipped_right - clipped_left),
                        .height = static_cast<uint32_t>(clipped_bottom - clipped_top)},
                       damage_policy_);
}

bool AppSurfaceCompositor::RenderDamage(PixelSurface destination, const AppDrawOperation* operations,
                                        uint32_t operation_count, uint32_t background, const AppLayerState& layer,
                                        bool use_layer_snapshot, uint32_t& draw_operations_replayed) {
    draw_operations_replayed = 0U;
    for (size_t damage_index = 0U; damage_index < damage_.Size(); ++damage_index) {
        const DamageRect damage = damage_[damage_index].rect;
        PixelSurface target = SubSurface(destination, damage);
        const SurfaceRect local_bounds{
            .x = 0,
            .y = 0,
            .width = static_cast<int32_t>(damage.width),
            .height = static_cast<int32_t>(damage.height),
        };
        if (!pixels_.Fill(target, local_bounds, background, 255U)) {
            return false;
        }
        const SurfaceRect global_bounds{.x = static_cast<int32_t>(damage.x),
                                        .y = static_cast<int32_t>(damage.y),
                                        .width = static_cast<int32_t>(damage.width),
                                        .height = static_cast<int32_t>(damage.height)};
        for (uint32_t index = 0U; index < operation_count; ++index) {
            const AppDrawOperation& operation = operations[index];
            if (!operation.visible || (use_layer_snapshot && operation.in_layer) ||
                !Intersects(operation.bounds, global_bounds)) {
                continue;
            }
            const SurfaceRect draw_bounds = Intersection(operation.bounds, global_bounds);
            PixelSurface operation_target =
                SubSurface(destination, {.x = static_cast<uint32_t>(draw_bounds.x),
                                         .y = static_cast<uint32_t>(draw_bounds.y),
                                         .width = static_cast<uint32_t>(draw_bounds.width),
                                         .height = static_cast<uint32_t>(draw_bounds.height)});
            const SurfaceRect local_destination = Translate(operation.destination, draw_bounds.x, draw_bounds.y);
            bool rendered = false;
            if (operation.kind == AppDrawOperationKind::kFill) {
                rendered = pixels_.Fill(operation_target,
                                        {.x = 0, .y = 0, .width = draw_bounds.width, .height = draw_bounds.height},
                                        operation.rgb888, operation.opacity);
            } else if (operation.kind == AppDrawOperationKind::kTexture) {
                ConstPixelSurface source{};
                SurfaceRect clipped_source{};
                SurfaceRect clipped_destination{};
                rendered = BitmapSurface(operation.bitmap, source) &&
                           (CropBlit(operation, draw_bounds, clipped_source, clipped_destination)
                                ? pixels_.Blit(source, clipped_source, operation_target, clipped_destination,
                                               operation.opacity)
                                : pixels_.Blit(source, operation.source, operation_target, local_destination,
                                               operation.opacity));
            } else {
                rendered = text_ != nullptr &&
                           text_->Draw(operation_target, local_destination.x, local_destination.y, operation.rgb888,
                                       operation.font, operation.text, operation.text_length);
            }
            if (!rendered) {
                return false;
            }
            ++draw_operations_replayed;
        }
        if (use_layer_snapshot) {
            SurfaceRect translated_layer{};
            if (!LayerRect(layer, translated_layer)) {
                return false;
            }
            const SurfaceRect draw_bounds = Intersection(translated_layer, global_bounds);
            if (draw_bounds.width > 0 && draw_bounds.height > 0) {
                PixelSurface operation_target =
                    SubSurface(destination, {.x = static_cast<uint32_t>(draw_bounds.x),
                                             .y = static_cast<uint32_t>(draw_bounds.y),
                                             .width = static_cast<uint32_t>(draw_bounds.width),
                                             .height = static_cast<uint32_t>(draw_bounds.height)});
                const SurfaceRect source{
                    .x = layer.clip.x + draw_bounds.x - translated_layer.x,
                    .y = layer.clip.y + draw_bounds.y - translated_layer.y,
                    .width = draw_bounds.width,
                    .height = draw_bounds.height,
                };
                const SurfaceRect local_destination{
                    .x = 0,
                    .y = 0,
                    .width = draw_bounds.width,
                    .height = draw_bounds.height,
                };
                if (!pixels_.Blit(layer_cache_.ReadOnly(), source, operation_target, local_destination, 255U)) {
                    return false;
                }
                ++draw_operations_replayed;
            }
        }
    }
    return true;
}

bool AppSurfaceCompositor::CaptureLayer(PixelSurface destination, const AppLayerState& layer) {
    SurfaceRect source{};
    return ValidDestination(layer_cache_) && layer_cache_.width == destination.width &&
           layer_cache_.height == destination.height && layer_cache_.format == destination.format &&
           LayerRect(layer, source) && pixels_.Blit(destination.ReadOnly(), source, layer_cache_, layer.clip, 255U);
}

void AppSurfaceCompositor::SetLayerCache(PixelSurface cache) {
    layer_cache_ = cache;
    layer_snapshot_active_ = false;
}

bool AppSurfaceCompositor::SameDestination(PixelSurface destination) const {
    return destination_pixels_ == destination.pixels && destination_size_ == destination.size &&
           destination_width_ == destination.width && destination_height_ == destination.height &&
           destination_stride_ == destination.stride && destination_format_ == destination.format &&
           destination_origin_x_ == destination.origin_x && destination_origin_y_ == destination.origin_y;
}

void AppSurfaceCompositor::RememberDestination(PixelSurface destination) {
    destination_pixels_ = destination.pixels;
    destination_size_ = destination.size;
    destination_width_ = destination.width;
    destination_height_ = destination.height;
    destination_stride_ = destination.stride;
    destination_format_ = destination.format;
    destination_origin_x_ = destination.origin_x;
    destination_origin_y_ = destination.origin_y;
}

AppSurfaceFrameResult AppSurfaceCompositor::Result(AppSurfaceStatus status, bool visual_changed,
                                                   uint32_t draw_operations_replayed) const {
    return {
        .status = status,
        .visual_changed = visual_changed,
        .layer_snapshot_used = layer_snapshot_active_,
        .damage_region_count = static_cast<uint32_t>(damage_.Size()),
        .capacity_merge_count = damage_.CapacityMergeCount(),
        .damage_pixels = DamagePixels(damage_),
        .draw_operations_replayed = draw_operations_replayed,
        .operations_normalized = normalized_operations_,
        .incremental_normalization = incremental_normalization_,
    };
}

AppSurfaceFrameResult AppSurfaceCompositor::PresentScene(const GuestScene& scene, PixelSurface destination,
                                                         device::BitmapResolver resolver, void* resolver_context) {
    damage_.Clear();
    normalized_operations_ = 0U;
    incremental_normalization_ = false;
    if (!ValidDestination(destination)) {
        return Result(AppSurfaceStatus::kInvalidArgument, false);
    }
    uint32_t scratch_count = 0U;
    AppLayerState scratch_layer{};
    bool incremental = synchronized_ && !scene.LastApplyWasKeyframe();
    for (uint16_t node_index = 0U; incremental && node_index < scene.NodeCount(); ++node_index) {
        const uint32_t changes = scene.NodeChanges(node_index);
        incremental = (changes & MICROPIXEL_GRAPHICS_SCENE_NODE_KIND) == 0U &&
                      !(scene.Nodes()[node_index].kind == GuestSceneNodeKind::kSpriteBatch &&
                        (changes & MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT) != 0U);
    }
    if (!incremental) {
        stale_operation_count_ = 0U;
        scratch_synchronized_ = false;
    }
    const AppSurfaceStatus normalize_status =
        incremental ? NormalizeScenePatch(scene, resolver, resolver_context, scratch_count, scratch_layer)
                    : NormalizeScene(scene, resolver, resolver_context, scratch_count, scratch_layer);
    incremental_normalization_ = incremental;
    if (!incremental && normalize_status == AppSurfaceStatus::kOk) {
        normalized_operations_ = scratch_count;
    }
    if (normalize_status != AppSurfaceStatus::kOk) {
        stale_operation_count_ = 0U;
        scratch_synchronized_ = false;
        return Result(normalize_status, false);
    }
    const AppSurfaceFrameResult result =
        PresentNormalized(scratch_count, scene.Background(), true, scratch_layer, destination);
    if (result.status != AppSurfaceStatus::kOk || !incremental) {
        stale_operation_count_ = 0U;
        scratch_synchronized_ = false;
    }
    return result;
}

AppSurfaceFrameResult AppSurfaceCompositor::PresentNormalized(uint32_t scratch_count, uint32_t scratch_background,
                                                              bool scratch_background_valid,
                                                              const AppLayerState& scratch_layer,
                                                              PixelSurface destination) {
    const bool background_changed = background_valid_ != scratch_background_valid ||
                                    (background_valid_ && background_rgb888_ != scratch_background);
    bool full_redraw = !synchronized_ || !SameDestination(destination) || background_changed;
    const bool same_layer = SameLayerDefinition(current_layer_, scratch_layer);
    const bool layer_content_changed = same_layer && LayerContentChanged(current_, current_count_, current_layer_,
                                                                         scratch_, scratch_count, scratch_layer);
    bool use_layer_snapshot =
        layer_snapshot_active_ && scratch_layer.translation_active && same_layer && !layer_content_changed;
    if (!use_layer_snapshot && scratch_layer.translation_active && synchronized_ && SameDestination(destination) &&
        current_layer_.valid && same_layer && !layer_content_changed && CaptureLayer(destination, current_layer_)) {
        use_layer_snapshot = true;
    }
    const bool leaving_layer_snapshot = layer_snapshot_active_ && !use_layer_snapshot;
    const bool skip_layer_differences = use_layer_snapshot || leaving_layer_snapshot;
    if (full_redraw) {
        SurfaceRect full{};
        if (!FullSurfaceRect(destination, full) || !AddDamage(destination, full)) {
            return Result(AppSurfaceStatus::kResourceExhausted, false);
        }
    } else {
        const uint32_t compared_count = current_count_ > scratch_count ? current_count_ : scratch_count;
        for (uint32_t index = 0U; index < compared_count; ++index) {
            const bool old_exists = index < current_count_;
            const bool new_exists = index < scratch_count;
            if (skip_layer_differences &&
                ((old_exists && current_[index].in_layer) || (new_exists && scratch_[index].in_layer))) {
                continue;
            }
            if (old_exists && new_exists && SameVisual(current_[index], scratch_[index])) {
                continue;
            }
            if (old_exists && current_[index].visible && !AddDamage(destination, current_[index].bounds)) {
                return Result(AppSurfaceStatus::kResourceExhausted, false);
            }
            if (new_exists && scratch_[index].visible && !AddDamage(destination, scratch_[index].bounds)) {
                return Result(AppSurfaceStatus::kResourceExhausted, false);
            }
        }
        if (skip_layer_differences) {
            SurfaceRect old_layer_rect{};
            SurfaceRect new_layer_rect{};
            const bool old_layer_visible = LayerRect(current_layer_, old_layer_rect);
            const bool new_layer_visible = LayerRect(scratch_layer, new_layer_rect);
            const bool layer_position_changed = old_layer_visible != new_layer_visible ||
                                                (old_layer_visible && !SameRect(old_layer_rect, new_layer_rect)) ||
                                                leaving_layer_snapshot;
            if (layer_position_changed && ((old_layer_visible && !AddDamage(destination, old_layer_rect)) ||
                                           (new_layer_visible && !AddDamage(destination, new_layer_rect)))) {
                return Result(AppSurfaceStatus::kResourceExhausted, false);
            }
        }
    }

    uint32_t draw_operations_replayed = 0U;
    if (!damage_.Empty() && !RenderDamage(destination, scratch_, scratch_count, scratch_background, scratch_layer,
                                          use_layer_snapshot, draw_operations_replayed)) {
        synchronized_ = false;
        layer_snapshot_active_ = false;
        return Result(AppSurfaceStatus::kRenderFailed, false, draw_operations_replayed);
    }
    std::swap(current_, scratch_);
    current_count_ = scratch_count;
    background_rgb888_ = scratch_background;
    background_valid_ = scratch_background_valid;
    current_layer_ = scratch_layer;
    layer_snapshot_active_ = use_layer_snapshot;
    synchronized_ = true;
    RememberDestination(destination);
    return Result(AppSurfaceStatus::kOk, !damage_.Empty(), draw_operations_replayed);
}

AppSurfaceFrameResult AppSurfaceCompositor::RefreshBitmap(const uint8_t* bitmap_data, DamageRect source_damage,
                                                          PixelSurface destination) {
    damage_.Clear();
    normalized_operations_ = 0U;
    incremental_normalization_ = false;
    if (!ValidDestination(destination) || !synchronized_ || !SameDestination(destination) || bitmap_data == nullptr ||
        source_damage.width == 0U || source_damage.height == 0U) {
        return Result(AppSurfaceStatus::kInvalidArgument, false);
    }
    const uint64_t dirty_right = static_cast<uint64_t>(source_damage.x) + source_damage.width;
    const uint64_t dirty_bottom = static_cast<uint64_t>(source_damage.y) + source_damage.height;
    for (uint32_t index = 0U; index < current_count_; ++index) {
        const AppDrawOperation& operation = current_[index];
        if (!operation.visible || (layer_snapshot_active_ && operation.in_layer) ||
            operation.kind != AppDrawOperationKind::kTexture || operation.bitmap.data != bitmap_data) {
            continue;
        }
        const uint32_t source_left = static_cast<uint32_t>(operation.source.x);
        const uint32_t source_top = static_cast<uint32_t>(operation.source.y);
        const uint64_t source_right = static_cast<uint64_t>(source_left) + operation.source.width;
        const uint64_t source_bottom = static_cast<uint64_t>(source_top) + operation.source.height;
        const uint64_t clipped_left = source_damage.x > source_left ? source_damage.x : source_left;
        const uint64_t clipped_top = source_damage.y > source_top ? source_damage.y : source_top;
        const uint64_t clipped_right = dirty_right < source_right ? dirty_right : source_right;
        const uint64_t clipped_bottom = dirty_bottom < source_bottom ? dirty_bottom : source_bottom;
        if (clipped_right <= clipped_left || clipped_bottom <= clipped_top) {
            continue;
        }
        const uint64_t relative_left = clipped_left - source_left;
        const uint64_t relative_top = clipped_top - source_top;
        const uint64_t relative_right = clipped_right - source_left;
        const uint64_t relative_bottom = clipped_bottom - source_top;
        const int32_t destination_left =
            operation.destination.x + static_cast<int32_t>(relative_left * operation.destination.width /
                                                           static_cast<uint32_t>(operation.source.width));
        const int32_t destination_top =
            operation.destination.y + static_cast<int32_t>(relative_top * operation.destination.height /
                                                           static_cast<uint32_t>(operation.source.height));
        const int32_t destination_right =
            operation.destination.x +
            static_cast<int32_t>((relative_right * operation.destination.width + operation.source.width - 1U) /
                                 static_cast<uint32_t>(operation.source.width));
        const int32_t destination_bottom =
            operation.destination.y +
            static_cast<int32_t>((relative_bottom * operation.destination.height + operation.source.height - 1U) /
                                 static_cast<uint32_t>(operation.source.height));
        const SurfaceRect mapped{.x = destination_left,
                                 .y = destination_top,
                                 .width = destination_right - destination_left,
                                 .height = destination_bottom - destination_top};
        if (!AddDamage(destination, Intersection(mapped, operation.bounds))) {
            return Result(AppSurfaceStatus::kResourceExhausted, false);
        }
    }
    uint32_t draw_operations_replayed = 0U;
    if (!damage_.Empty() && !RenderDamage(destination, current_, current_count_, background_rgb888_, current_layer_,
                                          layer_snapshot_active_, draw_operations_replayed)) {
        synchronized_ = false;
        return Result(AppSurfaceStatus::kRenderFailed, false, draw_operations_replayed);
    }
    return Result(AppSurfaceStatus::kOk, !damage_.Empty(), draw_operations_replayed);
}

void AppSurfaceCompositor::Reset() {
    damage_.Clear();
    current_count_ = 0U;
    background_rgb888_ = 0U;
    background_valid_ = false;
    current_layer_ = {};
    layer_snapshot_active_ = false;
    synchronized_ = false;
    stale_operation_count_ = 0U;
    scratch_synchronized_ = false;
    normalized_operations_ = 0U;
    incremental_normalization_ = false;
    destination_pixels_ = nullptr;
    destination_size_ = 0U;
    destination_width_ = 0U;
    destination_height_ = 0U;
    destination_stride_ = 0U;
    destination_format_ = {};
    destination_origin_x_ = 0U;
    destination_origin_y_ = 0U;
}

}  // namespace micropixel::platform::graphics
