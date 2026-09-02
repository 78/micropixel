#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "abi/micropixel_abi.h"
#include "runtime/display_transform.hpp"

namespace {

std::vector<uint8_t> submitted_scene;
int32_t submit_status = MICROPIXEL_STATUS_OK;

}  // namespace

extern "C" int32_t micropixel_service_open(uint32_t service_id, uint32_t required_interface_version,
                                           micropixel_service_info_t* info_out, uint32_t info_capacity) {
    assert(service_id == MICROPIXEL_SERVICE_GRAPHICS);
    assert(required_interface_version ==
           MICROPIXEL_INTERFACE_VERSION(MICROPIXEL_GRAPHICS_INTERFACE_MAJOR, MICROPIXEL_GRAPHICS_INTERFACE_MINOR));
    assert(info_out != nullptr && info_capacity >= sizeof(*info_out));
    *info_out = {.size = sizeof(*info_out),
                 .interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
                 .interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR,
                 .handle = 1U,
                 .flags = 0U};
    return MICROPIXEL_STATUS_OK;
}

extern "C" int32_t micropixel_service_submit(micropixel_service_handle_t service, uint32_t channel_id,
                                             const uint8_t* bytes, uint32_t length) {
    assert(service == 1U && channel_id == MICROPIXEL_GRAPHICS_CHANNEL_SCENE && bytes != nullptr);
    if (submit_status == MICROPIXEL_STATUS_OK) {
        submitted_scene.assign(bytes, bytes + length);
    }
    return submit_status;
}

namespace micropixel::detail {

const DisplayTransform& CurrentDisplayTransform() {
    static constexpr DisplayTransform transform{.logical_width = 8U,
                                                .logical_height = 8U,
                                                .physical_width = 8U,
                                                .physical_height = 8U,
                                                .scale_numerator = 1U,
                                                .scale_denominator = 1U};
    return transform;
}

}  // namespace micropixel::detail

namespace micropixel::runtime {

[[noreturn]] void Panic(const char*, int32_t) { std::abort(); }

}  // namespace micropixel::runtime

// The lifecycle store is intentionally private to the Guest runtime. Including
// the implementation here tests its bounded dynamic storage, transactions and
// wire behavior without exposing test-only SDK entry points.
#include "../../guest/runtime/scene_graph.cpp"

namespace {

using micropixel::SceneNodeKind;
using micropixel::SceneState;

class TestContainer final : public micropixel::Container {
   public:
    constexpr TestContainer(SceneState* state, uint16_t id, uint32_t generation) : Container(state, id, generation) {}
};

micropixel_graphics_scene_header_t SubmittedHeader() {
    assert(submitted_scene.size() >= sizeof(micropixel_graphics_scene_header_t));
    micropixel_graphics_scene_header_t header{};
    std::memcpy(&header, submitted_scene.data(), sizeof(header));
    return header;
}

void ConfigureShape(SceneState& state, uint16_t slot, int32_t x) {
    auto& node = state.nodes[slot];
    node.kind = SceneNodeKind::kShape;
    node.dirty = MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY | MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE |
                 MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY | MICROPIXEL_GRAPHICS_SCENE_NODE_LAYER |
                 MICROPIXEL_GRAPHICS_SCENE_NODE_KIND;
    node.destination = {x, 0, 1, 1};
    node.color = micropixel::Color::White();
    node.visible = true;
}

void Present(SceneState& state) {
    state.BeginTransaction();
    assert(micropixel::EncodeAndSubmit(state, !state.valid) == MICROPIXEL_STATUS_OK);
    state.CommitTransaction();
}

void DestroyReusesSlotsAndPreservesDrawOrder() {
    SceneState state;
    state.Reset({.logical_width = 8U, .logical_height = 8U});
    const uint16_t first = state.AllocateNode({});
    const uint16_t middle = state.AllocateNode({});
    const uint16_t last = state.AllocateNode({});
    ConfigureShape(state, first, 1);
    ConfigureShape(state, middle, 2);
    ConfigureShape(state, last, 3);
    Present(state);
    assert(SubmittedHeader().kind == MICROPIXEL_GRAPHICS_SCENE_KEYFRAME);
    assert(SubmittedHeader().node_count == 3U);

    const uint32_t stale_generation = state.nodes[middle].generation;
    state.BeginTransaction();
    state.DestroyNode(middle, stale_generation);
    assert(!state.NodeValid(middle, stale_generation));
    assert(micropixel::EncodeAndSubmit(state, true) == MICROPIXEL_STATUS_OK);
    state.CommitTransaction();
    assert(SubmittedHeader().node_count == 2U);

    const uint16_t replacement = state.AllocateNode({});
    assert(replacement == middle);
    assert(!state.NodeValid(replacement, stale_generation));
    ConfigureShape(state, replacement, 4);
    Present(state);
    assert(state.nodes[first].wire_id == 0U);
    assert(state.nodes[last].wire_id == 1U);
    assert(state.nodes[replacement].wire_id == 2U);
}

void DestroyRollbackRestoresTheHandleAndNodeCount() {
    SceneState state;
    state.Reset({.logical_width = 8U, .logical_height = 8U});
    const uint16_t slot = state.AllocateNode({});
    ConfigureShape(state, slot, 1);
    const uint32_t generation = state.nodes[slot].generation;
    state.BeginTransaction();
    state.DestroyNode(slot, generation);
    assert(!state.NodeValid(slot, generation) && state.node_count == 0U);
    state.RollbackTransaction();
    assert(state.NodeValid(slot, generation) && state.node_count == 1U);
}

void SubmitFailureRollsBackDestruction() {
    SceneState state;
    state.Reset({.logical_width = 8U, .logical_height = 8U});
    const uint16_t slot = state.AllocateNode({});
    ConfigureShape(state, slot, 1);
    Present(state);
    const uint32_t generation = state.nodes[slot].generation;

    state.BeginTransaction();
    state.DestroyNode(slot, generation);
    submit_status = MICROPIXEL_STATUS_INTERNAL;
    assert(micropixel::EncodeAndSubmit(state, true) == MICROPIXEL_STATUS_INTERNAL);
    state.RollbackTransaction();
    submit_status = MICROPIXEL_STATUS_OK;
    assert(state.NodeValid(slot, generation) && state.node_count == 1U);
}

void RepeatedDestroyAndReuseDoesNotExhaustThePool() {
    SceneState state;
    state.Reset({.logical_width = 8U, .logical_height = 8U});
    uint32_t previous_generation = 0U;
    for (uint32_t iteration = 0U; iteration < 1024U; ++iteration) {
        const uint16_t slot = state.AllocateNode({});
        assert(slot == 0U);
        const uint32_t generation = state.nodes[slot].generation;
        assert(generation != previous_generation);
        assert(!state.NodeValid(slot, previous_generation));
        ConfigureShape(state, slot, static_cast<int32_t>(iteration % 8U));
        state.BeginTransaction();
        state.DestroyNode(slot, generation);
        state.CommitTransaction();
        assert(state.node_count == 0U && !state.NodeValid(slot, generation));
        previous_generation = generation;
    }
}

void StorageGrowsOnDemandAndResetInvalidatesHandles() {
    SceneState state;
    state.Reset({.logical_width = 8U, .logical_height = 8U});
    assert(state.nodes.empty());
    assert(state.instances.empty());
    assert(state.containers.size() == 1U);
    assert(state.node_undo.empty() && state.instance_undo.empty() && state.container_undo.empty());

    const uint16_t node = state.AllocateNode({});
    const uint16_t container = state.AllocateContainer({}, {});
    const uint32_t node_generation = state.nodes[node].generation;
    const uint32_t container_generation = state.containers[container].generation;
    assert(state.nodes.size() == 1U && state.containers.size() == 2U);

    state.Reset({.logical_width = 8U, .logical_height = 8U});
    assert(!state.NodeValid(node, node_generation));
    assert(!state.ContainerValid(container, container_generation));
    assert(state.AllocateNode({}) == node);
    assert(state.AllocateContainer({}, {}) == container);
    assert(state.nodes[node].generation != node_generation);
    assert(state.containers[container].generation != container_generation);
}

void ContainerDestroyCascadesAndCanRollback() {
    SceneState state;
    state.Reset({.logical_width = 8U, .logical_height = 8U});
    const uint16_t container = state.AllocateContainer({}, {});
    const uint32_t generation = state.containers[container].generation;
    const uint16_t first = state.AllocateNode({});
    const uint16_t second = state.AllocateNode({});
    state.nodes[first].parent_container_id = container;
    state.nodes[second].parent_container_id = container;
    ConfigureShape(state, first, 1);
    ConfigureShape(state, second, 2);

    state.BeginTransaction();
    state.DestroyContainer(container, generation);
    assert(!state.ContainerValid(container, generation) && state.node_count == 0U);
    state.RollbackTransaction();
    assert(state.ContainerValid(container, generation));
    assert(state.NodeValid(first, state.nodes[first].generation));
    assert(state.NodeValid(second, state.nodes[second].generation));
}

void SpriteBatchDestroyCompactsInstances() {
    SceneState state;
    state.Reset({.logical_width = 8U, .logical_height = 8U});
    const uint16_t first = state.AllocateNode({});
    const uint16_t second = state.AllocateNode({});
    state.nodes[first].kind = SceneNodeKind::kSpriteBatch;
    state.nodes[first].batch_instance_offset = 0U;
    state.nodes[first].batch_capacity = 2U;
    state.nodes[second].kind = SceneNodeKind::kSpriteBatch;
    state.nodes[second].batch_instance_offset = 2U;
    state.nodes[second].batch_capacity = 2U;
    state.batch_instance_count = 4U;
    state.instances.resize(state.batch_instance_count);
    state.instances[2].value.destination.x = 6;
    state.instances[3].value.destination.x = 7;

    state.BeginTransaction();
    state.DestroyNode(first, state.nodes[first].generation);
    assert(state.batch_instance_count == 2U);
    assert(state.nodes[second].batch_instance_offset == 0U);
    assert(state.instances[0].value.destination.x == 6);
    assert(state.instances[1].value.destination.x == 7);
    state.RollbackTransaction();
    assert(state.batch_instance_count == 4U);
    assert(state.nodes[second].batch_instance_offset == 2U);
}

void NestedContainerCoordinatesAreLocalToTheirParent() {
    micropixel::scene_active = true;
    micropixel::scene_storage.Reset({.logical_width = 8U, .logical_height = 8U});
    TestContainer scene{&micropixel::scene_storage, 0U, 0U};
    auto page = scene.CreateContainer({.translation = {2, 3}});
    auto dialog = page.CreateContainer({.translation = {-1, 2}});

    const micropixel::Point scene_point = dialog.ToScene({4, 1});
    assert(scene_point.x == 5 && scene_point.y == 6);
    const micropixel::Point local_point = dialog.ToLocal(scene_point);
    assert(local_point.x == 4 && local_point.y == 1);
    micropixel::scene_active = false;
}

void TransactionalCreationRollsBackWithoutAliasingReusedSlots() {
    micropixel::scene_active = true;
    micropixel::scene_storage.Reset({.logical_width = 8U, .logical_height = 8U});
    TestContainer scene{&micropixel::scene_storage, 0U, 0U};

    micropixel::scene_storage.BeginTransaction();
    auto rolled_back_shape = scene.CreateShape({0, 0, 1, 1}, micropixel::Color::White());
    auto rolled_back_container = scene.CreateContainer({.translation = {1, 2}});
    auto rolled_back_batch = rolled_back_container.CreateSpriteBatch(3U);
    assert(rolled_back_shape.valid() && rolled_back_container.valid() && rolled_back_batch.valid());
    micropixel::scene_storage.RollbackTransaction();
    assert(!rolled_back_shape.valid() && !rolled_back_container.valid() && !rolled_back_batch.valid());
    assert(micropixel::scene_storage.batch_instance_count == 0U);

    auto replacement_shape = scene.CreateShape({1, 1, 1, 1}, micropixel::Color::White());
    auto replacement_container = scene.CreateContainer({.translation = {2, 3}});
    assert(replacement_shape.valid() && replacement_container.valid());
    assert(!rolled_back_shape.valid() && !rolled_back_container.valid());
    micropixel::scene_active = false;
}

void TransactionalCreationPublishesOneAtomicKeyframe() {
    micropixel::scene_active = true;
    micropixel::scene_storage.Reset({.logical_width = 8U, .logical_height = 8U});
    TestContainer scene{&micropixel::scene_storage, 0U, 0U};

    micropixel::scene_storage.BeginTransaction();
    auto page = scene.CreateContainer({.translation = {2, 3}});
    auto shape = page.CreateShape({1, 1, 2, 2}, micropixel::Color::White());
    assert(page.valid() && shape.valid());
    assert(micropixel::EncodeAndSubmit(micropixel::scene_storage, true) == MICROPIXEL_STATUS_OK);
    micropixel::scene_storage.CommitTransaction();

    const auto header = SubmittedHeader();
    assert(header.kind == MICROPIXEL_GRAPHICS_SCENE_KEYFRAME);
    assert(header.container_count == 1U && header.node_count == 1U);
    assert(page.valid() && shape.valid());

    assert(page.Destroy().has_value());
    assert(!page.valid() && !shape.valid());
    const auto destroyed_header = SubmittedHeader();
    assert(destroyed_header.kind == MICROPIXEL_GRAPHICS_SCENE_KEYFRAME);
    assert(destroyed_header.container_count == 0U && destroyed_header.node_count == 0U);
    micropixel::scene_active = false;
}

}  // namespace

int main() {
    DestroyReusesSlotsAndPreservesDrawOrder();
    DestroyRollbackRestoresTheHandleAndNodeCount();
    SubmitFailureRollsBackDestruction();
    RepeatedDestroyAndReuseDoesNotExhaustThePool();
    StorageGrowsOnDemandAndResetInvalidatesHandles();
    ContainerDestroyCascadesAndCanRollback();
    SpriteBatchDestroyCompactsInstances();
    NestedContainerCoordinatesAreLocalToTheirParent();
    TransactionalCreationRollsBackWithoutAliasingReusedSlots();
    TransactionalCreationPublishesOneAtomicKeyframe();
    return 0;
}
