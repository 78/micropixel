#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#include "abi/micropixel_abi.h"
#include "platform/graphics/app_surface_compositor.hpp"

namespace graphics = micropixel::platform::graphics;

namespace {

constexpr graphics::DamageMergePolicy kNoOverdraw{
    .max_extra_pixels = 0U,
    .max_region_pixels = 4096U,
};
constexpr graphics::DamageMergePolicy kLocalMerge{
    .max_extra_pixels = 32U,
    .max_region_pixels = 4096U,
};
constexpr uint32_t kNodeMask = MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY |
                               MICROPIXEL_GRAPHICS_SCENE_NODE_LAYER | MICROPIXEL_GRAPHICS_SCENE_NODE_KIND;
constexpr uint32_t kInstanceMask =
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_GEOMETRY | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_CONTENT |
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBILITY;
constexpr uint32_t kContainerMask =
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_CLIP | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION |
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_Z_ORDER |
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_STRUCTURE;

class SceneMessage final {
   public:
    SceneMessage(uint16_t kind, uint32_t generation, uint32_t base_revision, uint32_t revision, uint16_t node_count,
                 uint16_t batch_instance_count, uint16_t layer_count = 0U, uint16_t interface_minor = 1U)
        : kind_(kind),
          generation_(generation),
          base_revision_(base_revision),
          revision_(revision),
          node_count_(node_count),
          layer_count_(layer_count),
          batch_instance_count_(batch_instance_count),
          interface_minor_(interface_minor) {
        bytes_.resize(sizeof(micropixel_graphics_scene_header_t));
    }

    template <typename Record>
    void Add(const Record& record) {
        const size_t offset = bytes_.size();
        bytes_.resize(offset + sizeof(record));
        std::memcpy(bytes_.data() + offset, &record, sizeof(record));
        ++record_count_;
    }

    void AddInstances(uint16_t batch, uint16_t first, uint32_t property_mask,
                      const micropixel_graphics_scene_sprite_instance_t* instances, uint16_t count) {
        micropixel_graphics_scene_batch_instances_record_t record{
            .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_BATCH_INSTANCES,
                       .size = static_cast<uint16_t>(sizeof(record) + count * sizeof(instances[0]))},
            .batch_node_id = batch,
            .first_instance = first,
            .instance_count = count,
            .reserved0 = 0U,
            .property_mask = property_mask,
        };
        const size_t offset = bytes_.size();
        bytes_.resize(offset + record.record.size);
        std::memcpy(bytes_.data() + offset, &record, sizeof(record));
        std::memcpy(bytes_.data() + offset + sizeof(record), instances, count * sizeof(instances[0]));
        ++record_count_;
    }

    const std::vector<uint8_t>& Finish() {
        const micropixel_graphics_scene_header_t header{
            .magic = MICROPIXEL_GRAPHICS_SCENE_MAGIC,
            .interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
            .interface_minor = interface_minor_,
            .kind = kind_,
            .flags = 0U,
            .total_size = static_cast<uint32_t>(bytes_.size()),
            .generation = generation_,
            .base_revision = base_revision_,
            .revision = revision_,
            .record_count = record_count_,
            .node_count = node_count_,
            .layer_count = layer_count_,
            .batch_instance_count = batch_instance_count_,
        };
        std::memcpy(bytes_.data(), &header, sizeof(header));
        return bytes_;
    }

   private:
    uint16_t kind_{};
    uint32_t generation_{};
    uint32_t base_revision_{};
    uint32_t revision_{};
    uint16_t node_count_{};
    uint16_t layer_count_{};
    uint16_t batch_instance_count_{};
    uint16_t record_count_{};
    uint16_t interface_minor_{};
    std::vector<uint8_t> bytes_{};
};

template <size_t Size>
graphics::PixelSurface BgrSurface(std::array<uint8_t, Size>& pixels, uint32_t width, uint32_t height) {
    return {.pixels = pixels.data(),
            .size = static_cast<uint32_t>(pixels.size()),
            .width = width,
            .height = height,
            .stride = width * 3U,
            .format = graphics::SurfacePixelFormat::kBgr888};
}

class HardwareProbePixelCompositor final : public graphics::PixelCompositor {
   public:
    bool Fill(graphics::PixelSurface destination, graphics::SurfaceRect rect, uint32_t rgb888,
              uint8_t opacity) override {
        return software_.Fill(destination, rect, rgb888, opacity);
    }

    bool Blit(graphics::ConstPixelSurface source, graphics::SurfaceRect source_rect, graphics::PixelSurface destination,
              graphics::SurfaceRect destination_rect, uint8_t opacity) override {
        const int64_t right = static_cast<int64_t>(destination_rect.x) + destination_rect.width;
        const int64_t bottom = static_cast<int64_t>(destination_rect.y) + destination_rect.height;
        saw_unclipped_destination = saw_unclipped_destination || destination_rect.x < 0 || destination_rect.y < 0 ||
                                    right > destination.width || bottom > destination.height;
        return software_.Blit(source, source_rect, destination, destination_rect, opacity);
    }

    bool saw_unclipped_destination{};

   private:
    graphics::SoftwarePixelCompositor software_{};
};

struct Fixture final {
    HardwareProbePixelCompositor pixels{};
    std::array<graphics::AppDrawOperation, 16U> first_operations{};
    std::array<graphics::AppDrawOperation, 16U> second_operations{};
    std::array<graphics::GuestSceneNode, 16U> first_nodes{};
    std::array<graphics::GuestSceneNode, 16U> second_nodes{};
    std::array<graphics::GuestSceneSpriteInstance, 16U> first_instances{};
    std::array<graphics::GuestSceneSpriteInstance, 16U> second_instances{};
    std::array<graphics::GuestSceneContainer, MICROPIXEL_GRAPHICS_MAX_CONTAINERS + 1U> first_containers{};
    std::array<graphics::GuestSceneContainer, MICROPIXEL_GRAPHICS_MAX_CONTAINERS + 1U> second_containers{};
    std::array<uint16_t, 16U> first_draw_order{};
    std::array<uint16_t, 16U> second_draw_order{};
    graphics::GuestScene scene{
        first_nodes.data(),      second_nodes.data(),      static_cast<uint16_t>(first_nodes.size()),
        first_instances.data(),  second_instances.data(),  static_cast<uint16_t>(first_instances.size()),
        first_containers.data(), second_containers.data(), first_draw_order.data(),
        second_draw_order.data()};
    graphics::AppSurfaceCompositor compositor{first_operations.data(), second_operations.data(),
                                              static_cast<uint32_t>(first_operations.size()), pixels, kNoOverdraw};

    int32_t Apply(const std::vector<uint8_t>& bytes, int32_t width, int32_t height,
                  micropixel::device::BitmapResolver resolver = nullptr, void* context = nullptr) {
        return scene.Apply(bytes.data(), static_cast<uint32_t>(bytes.size()), width, height, resolver, context, nullptr,
                           nullptr);
    }
};

micropixel_graphics_scene_background_record_t Background(uint32_t color) {
    return {.record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_BACKGROUND,
                       .size = sizeof(micropixel_graphics_scene_background_record_t)},
            .property_mask = MICROPIXEL_GRAPHICS_SCENE_BACKGROUND_COLOR,
            .rgb888 = color};
}

micropixel_graphics_scene_layer_record_t Layer(int32_t translate_x, uint32_t property_mask) {
    return {.record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_LAYER,
                       .size = sizeof(micropixel_graphics_scene_layer_record_t)},
            .layer_id = 1U,
            .reserved0 = 0U,
            .property_mask = property_mask,
            .clip_x = 1,
            .clip_y = 1,
            .width = 3,
            .height = 2,
            .translate_x = translate_x,
            .translate_y = 0,
            .z_order = 0,
            .opacity = 255U,
            .visible = 1U};
}

micropixel_graphics_scene_rect_record_t Rect(uint16_t id, int32_t x, int32_t y, int32_t width, int32_t height,
                                             uint32_t color, uint8_t layer_id = 0U) {
    return {.node = {.record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_RECT,
                                .size = sizeof(micropixel_graphics_scene_rect_record_t)},
                     .node_id = id,
                     .layer_id = layer_id,
                     .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                     .property_mask = kNodeMask | MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY},
            .x = x,
            .y = y,
            .width = width,
            .height = height,
            .rgb888 = color,
            .opacity = 255U,
            .reserved0 = {}};
}

micropixel_graphics_scene_container_record_t Container(uint16_t id, uint16_t parent, uint16_t sibling,
                                                       int32_t translate_x, uint8_t opacity = 255U, bool visible = true,
                                                       uint32_t mask = kContainerMask) {
    return {
        .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_CONTAINER,
                   .size = sizeof(micropixel_graphics_scene_container_record_t)},
        .container_id = id,
        .parent_container_id = parent,
        .property_mask = mask,
        .clip_x = id == 1U ? 0 : 0,
        .clip_y = 0,
        .width = id == 1U ? 8 : 0,
        .height = id == 1U ? 2 : 0,
        .translate_x = translate_x,
        .translate_y = 0,
        .z_order = 0,
        .opacity = opacity,
        .visible = static_cast<uint8_t>(visible ? 1U : 0U),
        .sibling_order = sibling,
        .reserved0 = 0U,
    };
}

micropixel_graphics_scene_node_link_record_t Link(uint16_t node, uint16_t parent, uint16_t sibling) {
    return {
        .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_NODE_LINK,
                   .size = sizeof(micropixel_graphics_scene_node_link_record_t)},
        .node_id = node,
        .parent_container_id = parent,
        .sibling_order = sibling,
        .reserved0 = 0U,
    };
}

void InitialSceneRendersTheWholePersistentSurface() {
    Fixture fixture;
    std::array<uint8_t, 8U * 4U * 3U> storage{};
    auto surface = BgrSurface(storage, 8U, 4U);
    SceneMessage message(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 1U, 0U, 1U, 1U, 0U);
    message.Add(Background(0x010203U));
    message.Add(Rect(0U, 2, 1, 2, 2, 0xff0000U));
    const auto& bytes = message.Finish();
    assert(fixture.Apply(bytes, 8, 4) == MICROPIXEL_STATUS_OK);
    const auto result = fixture.compositor.PresentScene(fixture.scene, surface, nullptr, nullptr);
    assert(result.status == graphics::AppSurfaceStatus::kOk);
    assert(result.visual_changed && result.damage_pixels == 32U);
    assert(storage[0] == 0x03U && storage[1] == 0x02U && storage[2] == 0x01U);
    const size_t red = (1U * 8U + 2U) * 3U;
    assert(storage[red] == 0U && storage[red + 1U] == 0U && storage[red + 2U] == 255U);
}

void SmallerKeyframeClearsRemovedNodes() {
    Fixture fixture;
    std::array<uint8_t, 8U * 2U * 3U> storage{};
    auto surface = BgrSurface(storage, 8U, 2U);

    SceneMessage initial(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 1U, 0U, 1U, 2U, 0U);
    initial.Add(Background(0U));
    initial.Add(Rect(0U, 0, 0, 2, 1, 0xff0000U));
    initial.Add(Rect(1U, 4, 0, 2, 1, 0x00ff00U));
    assert(fixture.Apply(initial.Finish(), 8, 2) == MICROPIXEL_STATUS_OK);
    assert(fixture.compositor.PresentScene(fixture.scene, surface, nullptr, nullptr).status ==
           graphics::AppSurfaceStatus::kOk);
    const size_t removed_pixel = 4U * 3U;
    assert(storage[removed_pixel] == 0U && storage[removed_pixel + 1U] == 255U && storage[removed_pixel + 2U] == 0U);

    SceneMessage smaller(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 2U, 0U, 1U, 1U, 0U);
    smaller.Add(Background(0U));
    smaller.Add(Rect(0U, 0, 0, 2, 1, 0xff0000U));
    assert(fixture.Apply(smaller.Finish(), 8, 2) == MICROPIXEL_STATUS_OK);
    const auto result = fixture.compositor.PresentScene(fixture.scene, surface, nullptr, nullptr);
    assert(result.status == graphics::AppSurfaceStatus::kOk);
    assert(result.visual_changed && result.damage_pixels >= 2U);
    assert(storage[removed_pixel] == 0U && storage[removed_pixel + 1U] == 0U && storage[removed_pixel + 2U] == 0U);
}

void NestedContainersPropagateVisualStateWithOnePatch() {
    Fixture fixture;
    std::array<uint8_t, 8U * 2U * 3U> storage{};
    auto surface = BgrSurface(storage, 8U, 2U);

    SceneMessage keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 9U, 0U, 1U, 1U, 0U, 2U, 2U);
    keyframe.Add(Background(0U));
    keyframe.Add(Container(1U, 0U, 0U, 1, 128U));
    keyframe.Add(Container(2U, 1U, 1U, 1));
    keyframe.Add(Rect(0U, 0, 0, 2, 1, 0xff0000U));
    keyframe.Add(Link(0U, 2U, 2U));
    assert(fixture.Apply(keyframe.Finish(), 8, 2) == MICROPIXEL_STATUS_OK);
    const auto initial = fixture.compositor.PresentScene(fixture.scene, surface, nullptr, nullptr);
    assert(initial.status == graphics::AppSurfaceStatus::kOk);
    const size_t translated_pixel = 2U * 3U;
    assert(storage[translated_pixel] == 0U && storage[translated_pixel + 1U] == 0U &&
           storage[translated_pixel + 2U] >= 127U && storage[translated_pixel + 2U] <= 128U);

    SceneMessage patch(MICROPIXEL_GRAPHICS_SCENE_PATCH, 9U, 1U, 2U, 1U, 0U, 2U, 2U);
    patch.Add(Container(1U, 0U, 0U, 1, 128U, false, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_APPEARANCE));
    assert(fixture.Apply(patch.Finish(), 8, 2) == MICROPIXEL_STATUS_OK);
    const auto hidden = fixture.compositor.PresentScene(fixture.scene, surface, nullptr, nullptr);
    assert(hidden.status == graphics::AppSurfaceStatus::kOk && hidden.incremental_normalization);
    assert(storage[translated_pixel] == 0U && storage[translated_pixel + 1U] == 0U &&
           storage[translated_pixel + 2U] == 0U);
}

void SpriteBatchPatchDamagesOnlyOldTailAndNewHead() {
    Fixture fixture;
    std::array<uint8_t, 12U * 3U> storage{};
    auto surface = BgrSurface(storage, 12U, 1U);
    SceneMessage keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 1U, 0U, 1U, 1U, 2U);
    keyframe.Add(Background(0U));
    keyframe.Add(micropixel_graphics_scene_sprite_batch_record_t{
        .node = {.record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_SPRITE_BATCH,
                            .size = sizeof(micropixel_graphics_scene_sprite_batch_record_t)},
                 .node_id = 0U,
                 .layer_id = 0U,
                 .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                 .property_mask = kNodeMask | MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT},
        .texture = 0U,
        .capacity = 2U,
        .opacity = 255U,
        .reserved0 = 0U});
    const micropixel_graphics_scene_sprite_instance_t instances[2]{
        {.x = 0,
         .y = 0,
         .width = 2,
         .height = 1,
         .rgb888 = 0x00ff00U,
         .opacity = 255U,
         .flags = MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBLE},
        {.x = 4,
         .y = 0,
         .width = 2,
         .height = 1,
         .rgb888 = 0x00ff00U,
         .opacity = 255U,
         .flags = MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBLE}};
    keyframe.AddInstances(0U, 0U, kInstanceMask, instances, 2U);
    assert(fixture.Apply(keyframe.Finish(), 12, 1) == MICROPIXEL_STATUS_OK);
    assert(fixture.compositor.PresentScene(fixture.scene, surface, nullptr, nullptr).status ==
           graphics::AppSurfaceStatus::kOk);

    SceneMessage patch(MICROPIXEL_GRAPHICS_SCENE_PATCH, 1U, 1U, 2U, 1U, 2U);
    const micropixel_graphics_scene_sprite_instance_t new_head{.x = 8,
                                                               .y = 0,
                                                               .width = 2,
                                                               .height = 1,
                                                               .rgb888 = 0x00ff00U,
                                                               .opacity = 255U,
                                                               .flags = MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBLE};
    patch.AddInstances(0U, 0U, MICROPIXEL_GRAPHICS_SCENE_INSTANCE_GEOMETRY, &new_head, 1U);
    assert(fixture.Apply(patch.Finish(), 12, 1) == MICROPIXEL_STATUS_OK);
    const auto result = fixture.compositor.PresentScene(fixture.scene, surface, nullptr, nullptr);
    assert(result.status == graphics::AppSurfaceStatus::kOk);
    assert(result.incremental_normalization && result.operations_normalized == 1U);
    assert(result.damage_region_count == 2U && result.damage_pixels == 4U);
    assert(!fixture.pixels.saw_unclipped_destination);
    assert(storage[1U] == 0U);
    assert(storage[8U * 3U + 1U] == 255U);
}

bool ResolveBitmap(void* context, micropixel_texture_handle_t texture, micropixel::device::BitmapView& view) {
    if (context == nullptr || texture != 7U) {
        return false;
    }
    view = *static_cast<micropixel::device::BitmapView*>(context);
    return true;
}

void StreamingSurfaceDamageMapsToItsDestination() {
    Fixture fixture;
    std::array<uint8_t, 4U * 3U> bitmap_pixels{0U, 0U, 255U, 0U, 255U, 0U, 255U, 0U, 0U, 255U, 255U, 255U};
    micropixel::device::BitmapView bitmap{.data = bitmap_pixels.data(),
                                          .size = static_cast<uint32_t>(bitmap_pixels.size()),
                                          .width = 4U,
                                          .height = 1U,
                                          .stride = 12U,
                                          .pixel_format = MICROPIXEL_PIXEL_FORMAT_BGR888,
                                          .flags = MICROPIXEL_TEXTURE_FLAG_STREAMING};
    std::array<uint8_t, 8U * 3U> storage{};
    auto surface = BgrSurface(storage, 8U, 1U);
    SceneMessage message(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 1U, 0U, 1U, 1U, 0U);
    message.Add(Background(0U));
    message.Add(micropixel_graphics_scene_texture_record_t{
        .node = {.record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_TEXTURE,
                            .size = sizeof(micropixel_graphics_scene_texture_record_t)},
                 .node_id = 0U,
                 .layer_id = 0U,
                 .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                 .property_mask =
                     kNodeMask | MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY | MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT},
        .x = 0,
        .y = 0,
        .width = 8,
        .height = 1,
        .texture = 7U,
        .source_x = 0,
        .source_y = 0,
        .source_width = 4,
        .source_height = 1,
        .opacity = 255U,
        .reserved0 = {}});
    assert(fixture.Apply(message.Finish(), 8, 1, ResolveBitmap, &bitmap) == MICROPIXEL_STATUS_OK);
    assert(fixture.compositor.PresentScene(fixture.scene, surface, ResolveBitmap, &bitmap).status ==
           graphics::AppSurfaceStatus::kOk);

    bitmap_pixels[3U] = 255U;
    bitmap_pixels[4U] = 255U;
    bitmap_pixels[5U] = 255U;
    const auto result =
        fixture.compositor.RefreshBitmap(bitmap_pixels.data(), {.x = 1U, .y = 0U, .width = 1U, .height = 1U}, surface);
    assert(result.status == graphics::AppSurfaceStatus::kOk);
    assert(result.damage_region_count == 1U && result.damage_pixels == 2U);
    assert(fixture.compositor.LastDamage(0U).x == 2U && fixture.compositor.LastDamage(0U).width == 2U);
}

void AtlasFramePatchDamagesOnlyTheSpriteBounds() {
    Fixture fixture;
    std::array<uint8_t, 4U * 3U> bitmap_pixels{
        0U, 0U, 255U, 0U, 0U, 255U, 0U, 255U, 0U, 0U, 255U, 0U,
    };
    micropixel::device::BitmapView bitmap{.data = bitmap_pixels.data(),
                                          .size = static_cast<uint32_t>(bitmap_pixels.size()),
                                          .width = 4U,
                                          .height = 1U,
                                          .stride = 12U,
                                          .pixel_format = MICROPIXEL_PIXEL_FORMAT_BGR888,
                                          .flags = 0U};
    std::array<uint8_t, 8U * 3U> storage{};
    auto surface = BgrSurface(storage, 8U, 1U);
    SceneMessage keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 1U, 0U, 1U, 1U, 0U);
    keyframe.Add(Background(0U));
    keyframe.Add(micropixel_graphics_scene_texture_record_t{
        .node = {.record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_TEXTURE,
                            .size = sizeof(micropixel_graphics_scene_texture_record_t)},
                 .node_id = 0U,
                 .layer_id = 0U,
                 .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                 .property_mask =
                     kNodeMask | MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY | MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT},
        .x = 3,
        .y = 0,
        .width = 2,
        .height = 1,
        .texture = 7U,
        .source_x = 0,
        .source_y = 0,
        .source_width = 2,
        .source_height = 1,
        .opacity = 255U,
        .reserved0 = {}});
    assert(fixture.Apply(keyframe.Finish(), 8, 1, ResolveBitmap, &bitmap) == MICROPIXEL_STATUS_OK);
    assert(fixture.compositor.PresentScene(fixture.scene, surface, ResolveBitmap, &bitmap).status ==
           graphics::AppSurfaceStatus::kOk);

    SceneMessage patch(MICROPIXEL_GRAPHICS_SCENE_PATCH, 1U, 1U, 2U, 1U, 0U);
    patch.Add(micropixel_graphics_scene_texture_record_t{
        .node = {.record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_TEXTURE,
                            .size = sizeof(micropixel_graphics_scene_texture_record_t)},
                 .node_id = 0U,
                 .layer_id = 0U,
                 .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                 .property_mask = MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT},
        .texture = 7U,
        .source_x = 2,
        .source_y = 0,
        .source_width = 2,
        .source_height = 1,
        .opacity = 255U,
        .reserved0 = {}});
    assert(fixture.Apply(patch.Finish(), 8, 1, ResolveBitmap, &bitmap) == MICROPIXEL_STATUS_OK);
    const auto result = fixture.compositor.PresentScene(fixture.scene, surface, ResolveBitmap, &bitmap);
    assert(result.status == graphics::AppSurfaceStatus::kOk);
    assert(result.damage_region_count == 1U && result.damage_pixels == 2U);
    assert(!fixture.pixels.saw_unclipped_destination);
    assert(fixture.compositor.LastDamage(0U).x == 3U && fixture.compositor.LastDamage(0U).width == 2U);
    assert(storage[3U * 3U + 1U] == 255U);
}

void LayerShakeUsesSnapshotButContentChangesRemainVisible() {
    Fixture fixture;
    std::array<uint8_t, 8U * 4U * 3U> storage{};
    std::array<uint8_t, 8U * 4U * 3U> cache_storage{};
    auto surface = BgrSurface(storage, 8U, 4U);
    auto cache = BgrSurface(cache_storage, 8U, 4U);
    fixture.compositor.SetLayerCache(cache);

    constexpr uint32_t kFullLayerMask =
        MICROPIXEL_GRAPHICS_SCENE_LAYER_CLIP | MICROPIXEL_GRAPHICS_SCENE_LAYER_TRANSLATION |
        MICROPIXEL_GRAPHICS_SCENE_LAYER_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_LAYER_Z_ORDER;
    SceneMessage keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 1U, 0U, 1U, 1U, 0U, 1U);
    keyframe.Add(Background(0U));
    keyframe.Add(Layer(0, kFullLayerMask));
    keyframe.Add(Rect(0U, 1, 1, 1, 1, 0x00ff00U, 1U));
    assert(fixture.Apply(keyframe.Finish(), 8, 4) == MICROPIXEL_STATUS_OK);
    assert(fixture.compositor.PresentScene(fixture.scene, surface, nullptr, nullptr).status ==
           graphics::AppSurfaceStatus::kOk);

    SceneMessage shake(MICROPIXEL_GRAPHICS_SCENE_PATCH, 1U, 1U, 2U, 1U, 0U, 1U);
    shake.Add(Layer(2, MICROPIXEL_GRAPHICS_SCENE_LAYER_TRANSLATION));
    assert(fixture.Apply(shake.Finish(), 8, 4) == MICROPIXEL_STATUS_OK);
    const auto translated = fixture.compositor.PresentScene(fixture.scene, surface, nullptr, nullptr);
    assert(translated.status == graphics::AppSurfaceStatus::kOk);
    assert(translated.incremental_normalization && translated.operations_normalized == 1U);
    assert(translated.damage_pixels == 10U && translated.draw_operations_replayed == 1U &&
           translated.layer_snapshot_used);
    assert(storage[(1U * 8U + 1U) * 3U + 1U] == 0U);
    assert(storage[(1U * 8U + 3U) * 3U + 1U] == 255U);

    SceneMessage animation(MICROPIXEL_GRAPHICS_SCENE_PATCH, 1U, 2U, 3U, 1U, 0U, 1U);
    auto animated_rect = Rect(0U, 2, 1, 1, 1, 0xff0000U, 1U);
    animated_rect.node.property_mask =
        MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY | MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE;
    animation.Add(animated_rect);
    assert(fixture.Apply(animation.Finish(), 8, 4) == MICROPIXEL_STATUS_OK);
    const auto animated = fixture.compositor.PresentScene(fixture.scene, surface, nullptr, nullptr);
    assert(animated.status == graphics::AppSurfaceStatus::kOk);
    assert(animated.damage_pixels == 6U && !animated.layer_snapshot_used);
    assert(storage[(1U * 8U + 4U) * 3U + 2U] == 255U);
}

void DamageRegionsMergeWithoutLosingSourceIdentity() {
    graphics::DamageRegionSet<4U> regions;
    const uint8_t source{};
    const uint8_t other_source{};
    assert(regions.Add(&source, {.x = 0U, .y = 0U, .width = 4U, .height = 4U}, kNoOverdraw));
    assert(regions.Add(&source, {.x = 8U, .y = 0U, .width = 4U, .height = 4U}, kNoOverdraw));
    assert(regions.Size() == 2U);
    assert(regions.Add(&source, {.x = 4U, .y = 0U, .width = 4U, .height = 4U}, kNoOverdraw));
    assert(regions.Size() == 1U && regions[0U].rect.width == 12U);

    regions.Clear();
    assert(regions.Add(&source, {.x = 4U, .y = 6U, .width = 8U, .height = 8U}, kLocalMerge));
    assert(regions.Add(&source, {.x = 12U, .y = 6U, .width = 4U, .height = 8U}, kLocalMerge));
    assert(regions.Size() == 1U && regions[0U].rect.x == 4U && regions[0U].rect.y == 6U &&
           regions[0U].rect.width == 12U && regions[0U].rect.height == 8U);
    assert(regions.Add(&other_source, {.x = 4U, .y = 6U, .width = 12U, .height = 8U}, kLocalMerge));
    assert(regions.Size() == 2U);
}

void DamageRegionsMergeLargeOverlapButNotUnrelatedSourcesAtCapacity() {
    graphics::DamageRegionSet<4U> overlapping;
    const uint8_t source{};
    assert(overlapping.Add(&source, {.x = 0U, .y = 0U, .width = 200U, .height = 200U}, kNoOverdraw));
    assert(overlapping.Add(&source, {.x = 4U, .y = 2U, .width = 200U, .height = 200U}, kNoOverdraw));
    assert(overlapping.Size() == 1U && overlapping[0U].rect.width == 204U && overlapping[0U].rect.height == 202U);

    graphics::DamageRegionSet<2U> unrelated;
    const uint8_t second_source{};
    const uint8_t third_source{};
    assert(unrelated.Add(&source, {.x = 0U, .y = 0U, .width = 4U, .height = 4U}, kLocalMerge));
    assert(unrelated.Add(&second_source, {.x = 0U, .y = 0U, .width = 4U, .height = 4U}, kLocalMerge));
    assert(!unrelated.Add(&third_source, {.x = 0U, .y = 0U, .width = 4U, .height = 4U}, kLocalMerge));
    assert(unrelated.Size() == 2U);
}

void DamageRegionCapacityUsesTheCheapestSafeUnion() {
    graphics::DamageRegionSet<2U> regions;
    const uint8_t source{};
    assert(regions.Add(&source, {.x = 0U, .y = 0U, .width = 2U, .height = 2U}, kNoOverdraw));
    assert(regions.Add(&source, {.x = 100U, .y = 0U, .width = 2U, .height = 2U}, kNoOverdraw));
    assert(regions.Add(&source, {.x = 10U, .y = 0U, .width = 2U, .height = 2U}, kNoOverdraw));
    assert(regions.Size() == 2U && regions.CapacityMergeCount() == 1U);
    assert(regions[0U].rect.x == 0U && regions[0U].rect.width == 12U && regions[1U].rect.x == 100U);

    regions.Clear();
    const uint8_t other_source{};
    assert(regions.Add(&source, {.x = 0U, .y = 0U, .width = 2U, .height = 2U}, kNoOverdraw));
    assert(regions.Add(&source, {.x = 100U, .y = 0U, .width = 2U, .height = 2U}, kNoOverdraw));
    assert(regions.Add(&other_source, {.x = 5U, .y = 5U, .width = 2U, .height = 2U}, kNoOverdraw));
    assert(regions.Size() == 2U && regions.CapacityMergeCount() == 1U);
    assert(regions[0U].source == &source && regions[0U].rect.width == 102U);
    assert(regions[1U].source == &other_source);
}

void DamageRegionsRejectInvalidRectanglesAndResetMetrics() {
    graphics::DamageRegionSet<1U> regions;
    const uint8_t source{};
    assert(!regions.Add(nullptr, {.x = 0U, .y = 0U, .width = 1U, .height = 1U}, kLocalMerge));
    assert(!regions.Add(&source, {.x = 0U, .y = 0U, .width = 0U, .height = 1U}, kLocalMerge));
    assert(!regions.Add(&source, {.x = UINT32_MAX, .y = 0U, .width = 2U, .height = 1U}, kLocalMerge));
    assert(regions.Empty());

    assert(regions.Add(&source, {.x = 0U, .y = 0U, .width = 1U, .height = 1U}, kNoOverdraw));
    assert(regions.Add(&source, {.x = 10U, .y = 0U, .width = 1U, .height = 1U}, kNoOverdraw));
    assert(regions.CapacityMergeCount() == 1U);
    regions.Clear();
    assert(regions.Empty() && regions.CapacityMergeCount() == 0U);
}

}  // namespace

int main() {
    InitialSceneRendersTheWholePersistentSurface();
    SmallerKeyframeClearsRemovedNodes();
    NestedContainersPropagateVisualStateWithOnePatch();
    SpriteBatchPatchDamagesOnlyOldTailAndNewHead();
    StreamingSurfaceDamageMapsToItsDestination();
    AtlasFramePatchDamagesOnlyTheSpriteBounds();
    LayerShakeUsesSnapshotButContentChangesRemainVisible();
    DamageRegionsMergeWithoutLosingSourceIdentity();
    DamageRegionsMergeLargeOverlapButNotUnrelatedSourcesAtCapacity();
    DamageRegionCapacityUsesTheCheapestSafeUnion();
    DamageRegionsRejectInvalidRectanglesAndResetMetrics();
    return 0;
}
