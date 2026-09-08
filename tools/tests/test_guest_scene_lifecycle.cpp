#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "abi/micropixel_abi.h"
#include "runtime/display_transform.hpp"
#include "runtime/graphics_limits.hpp"
#include "sdk/application.hpp"

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
    *info_out = {.size = sizeof(*info_out), .service_handle = 1U, .flags = 0U};
    return MICROPIXEL_STATUS_OK;
}

extern "C" int32_t micropixel_service_submit(micropixel_service_handle_t service_handle, uint32_t channel_id,
                                             const uint8_t* bytes, uint32_t length) {
    assert(service_handle == 1U && channel_id == MICROPIXEL_GRAPHICS_CHANNEL_SCENE && bytes != nullptr);
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

// The Host under test grants the Guest's full static capacity.
const GraphicsLimits& LoadGraphicsLimits() {
    static constexpr GraphicsLimits kLimits{.max_text_bytes = limits::kMaxTextBytes,
                                            .max_scene_bytes = limits::kMaxSceneBytes,
                                            .max_raster_bytes = limits::kMaxRasterBytes,
                                            .max_surface_buffers = limits::kMaxSurfaceBuffers};
    return kLimits;
}

}  // namespace micropixel::runtime

namespace micropixel {
Application::Application() noexcept = default;
}

// The lifecycle store is intentionally private to the Guest runtime. Including
// the implementation here tests its bounded dynamic storage, pending changes and
// wire behavior without exposing test-only SDK entry points.
#include "../../guest/runtime/scene_graph.cpp"

namespace micropixel {
Texture::~Texture() = default;
}

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
                 MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY | MICROPIXEL_GRAPHICS_SCENE_NODE_KIND;
    node.destination = {x, 0, 1, 1};
    node.color = micropixel::Color::White();
    node.visible = true;
}

void Present(SceneState& state) {
    state.BeginFrame();
    assert(micropixel::EncodeAndSubmit(state, !state.valid) == MICROPIXEL_STATUS_OK);
    state.AcceptFrame();
}

void DestroyReusesSlotsAndPreservesDrawOrder() {
    SceneState state;
    state.Reset({.logical_width = 8U, .logical_height = 8U});
    const uint16_t first = state.AllocateNode({}).value();
    const uint16_t middle = state.AllocateNode({}).value();
    const uint16_t last = state.AllocateNode({}).value();
    ConfigureShape(state, first, 1);
    ConfigureShape(state, middle, 2);
    ConfigureShape(state, last, 3);
    Present(state);
    assert(SubmittedHeader().kind == MICROPIXEL_GRAPHICS_SCENE_KEYFRAME);
    assert(SubmittedHeader().node_count == 3U);

    const uint32_t stale_generation = state.nodes[middle].generation;
    state.BeginFrame();
    state.DestroyNode(middle, stale_generation);
    assert(!state.NodeValid(middle, stale_generation));
    assert(micropixel::EncodeAndSubmit(state, true) == MICROPIXEL_STATUS_OK);
    state.AcceptFrame();
    assert(SubmittedHeader().node_count == 2U);

    const uint16_t replacement = state.AllocateNode({}).value();
    assert(replacement == middle);
    assert(!state.NodeValid(replacement, stale_generation));
    ConfigureShape(state, replacement, 4);
    Present(state);
    assert(state.nodes[first].wire_id == 0U);
    assert(state.nodes[last].wire_id == 1U);
    assert(state.nodes[replacement].wire_id == 2U);
}

void DestroyedHandleStaysInvalidAcrossFrames() {
    SceneState state;
    state.Reset({.logical_width = 8U, .logical_height = 8U});
    const uint16_t slot = state.AllocateNode({}).value();
    ConfigureShape(state, slot, 1);
    const uint32_t generation = state.nodes[slot].generation;
    state.BeginFrame();
    state.DestroyNode(slot, generation);
    assert(!state.NodeValid(slot, generation) && state.node_count == 0U);
    state.AcceptFrame();
    assert(!state.NodeValid(slot, generation) && state.node_count == 0U);
}

void SubmitFailureKeepsDestroyedNode() {
    SceneState state;
    state.Reset({.logical_width = 8U, .logical_height = 8U});
    const uint16_t slot = state.AllocateNode({}).value();
    ConfigureShape(state, slot, 1);
    Present(state);
    const uint32_t generation = state.nodes[slot].generation;

    state.BeginFrame();
    state.DestroyNode(slot, generation);
    submit_status = MICROPIXEL_STATUS_INTERNAL;
    assert(micropixel::EncodeAndSubmit(state, true) == MICROPIXEL_STATUS_INTERNAL);
    submit_status = MICROPIXEL_STATUS_OK;
    assert(!state.NodeValid(slot, generation) && state.node_count == 0U);
    assert(micropixel::EncodeAndSubmit(state, true) == MICROPIXEL_STATUS_OK);
    state.AcceptFrame();
    assert(SubmittedHeader().node_count == 0);
}

void RepeatedDestroyAndReuseDoesNotExhaustThePool() {
    SceneState state;
    state.Reset({.logical_width = 8U, .logical_height = 8U});
    uint32_t previous_generation = 0U;
    for (uint32_t iteration = 0U; iteration < 1024U; ++iteration) {
        const uint16_t slot = state.AllocateNode({}).value();
        assert(slot == 0U);
        const uint32_t generation = state.nodes[slot].generation;
        assert(generation != previous_generation);
        assert(!state.NodeValid(slot, previous_generation));
        ConfigureShape(state, slot, static_cast<int32_t>(iteration % 8U));
        state.BeginFrame();
        state.DestroyNode(slot, generation);
        state.AcceptFrame();
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

    const uint16_t node = state.AllocateNode({}).value();
    const uint16_t container = state.AllocateContainer({}, {}).value();
    const uint32_t node_generation = state.nodes[node].generation;
    const uint32_t container_generation = state.containers[container].generation;
    assert(state.nodes.size() == 1U && state.containers.size() == 2U);

    state.Reset({.logical_width = 8U, .logical_height = 8U});
    assert(!state.NodeValid(node, node_generation));
    assert(!state.ContainerValid(container, container_generation));
    assert(state.AllocateNode({}).value() == node);
    assert(state.AllocateContainer({}, {}).value() == container);
    assert(state.nodes[node].generation != node_generation);
    assert(state.containers[container].generation != container_generation);
}

void ContainerDestroyCascadesPermanently() {
    SceneState state;
    state.Reset({.logical_width = 8U, .logical_height = 8U});
    const uint16_t container = state.AllocateContainer({}, {}).value();
    const uint32_t generation = state.containers[container].generation;
    const uint16_t first = state.AllocateNode({}).value();
    const uint16_t second = state.AllocateNode({}).value();
    state.nodes[first].parent_container_id = container;
    state.nodes[second].parent_container_id = container;
    ConfigureShape(state, first, 1);
    ConfigureShape(state, second, 2);

    state.BeginFrame();
    state.DestroyContainer(container, generation);
    assert(!state.ContainerValid(container, generation) && state.node_count == 0U);
    state.AcceptFrame();
    assert(!state.ContainerValid(container, generation));
    assert(!state.NodeValid(first, state.nodes[first].generation));
    assert(!state.NodeValid(second, state.nodes[second].generation));
}

void SpriteBatchDestroyCompactsInstances() {
    SceneState state;
    state.Reset({.logical_width = 8U, .logical_height = 8U});
    const uint16_t first = state.AllocateNode({}).value();
    const uint16_t second = state.AllocateNode({}).value();
    state.nodes[first].kind = SceneNodeKind::kSpriteBatch;
    state.nodes[first].batch_instance_offset = 0U;
    state.nodes[first].batch_capacity = 2U;
    state.nodes[second].kind = SceneNodeKind::kSpriteBatch;
    state.nodes[second].batch_instance_offset = 2U;
    state.nodes[second].batch_capacity = 2U;
    state.batch_instance_count = 4U;
    assert(state.instances.Reserve(state.batch_instance_count));
    assert(state.instance_undo.Reserve(state.batch_instance_count));
    assert(state.instance_undo_slot.Reserve(state.batch_instance_count));
    state.instances.resize(state.batch_instance_count);
    state.instances[2].value.destination.x = 6;
    state.instances[3].value.destination.x = 7;

    state.BeginFrame();
    state.DestroyNode(first, state.nodes[first].generation);
    assert(state.batch_instance_count == 2U);
    assert(state.nodes[second].batch_instance_offset == 0U);
    assert(state.instances[0].value.destination.x == 6);
    assert(state.instances[1].value.destination.x == 7);
    state.AcceptFrame();
    assert(state.batch_instance_count == 2U);
    assert(state.nodes[second].batch_instance_offset == 0U);
}

void NestedContainerCoordinatesAreLocalToTheirParent() {
    micropixel::scene_active = true;
    micropixel::scene_storage.Reset({.logical_width = 8U, .logical_height = 8U});
    TestContainer scene{&micropixel::scene_storage, 0U, 0U};
    auto page = scene.CreateContainer({.translation = {2, 3}}).value();
    auto dialog = page.CreateContainer({.translation = {-1, 2}}).value();

    const micropixel::Point scene_point = dialog.ToScene({4, 1});
    assert(scene_point.x == 5 && scene_point.y == 6);
    const micropixel::Point local_point = dialog.ToLocal(scene_point);
    assert(local_point.x == 4 && local_point.y == 1);
    micropixel::scene_active = false;
}

void CreationFailureAndDestructionDoNotAliasReusedSlots() {
    micropixel::scene_active = true;
    micropixel::scene_storage.Reset({.logical_width = 8U, .logical_height = 8U});
    TestContainer scene{&micropixel::scene_storage, 0U, 0U};

    micropixel::scene_storage.BeginFrame();
    auto created_shape = scene.CreateShape({0, 0, 1, 1}, micropixel::Color::White()).value();
    auto created_container = scene.CreateContainer({.translation = {1, 2}}).value();
    auto created_batch = created_container.CreateSpriteBatch(3U).value();
    assert(created_shape.valid() && created_container.valid() && created_batch.valid());
    submit_status = MICROPIXEL_STATUS_INTERNAL;
    assert(micropixel::EncodeAndSubmit(micropixel::scene_storage, true) == MICROPIXEL_STATUS_INTERNAL);
    submit_status = MICROPIXEL_STATUS_OK;
    assert(created_shape.valid() && created_container.valid() && created_batch.valid());
    created_shape.Destroy();
    assert(created_container.Destroy());
    assert(!created_shape.valid() && !created_container.valid() && !created_batch.valid());
    assert(micropixel::scene_storage.batch_instance_count == 0U);

    auto replacement_shape = scene.CreateShape({1, 1, 1, 1}, micropixel::Color::White()).value();
    auto replacement_container = scene.CreateContainer({.translation = {2, 3}}).value();
    assert(replacement_shape.valid() && replacement_container.valid());
    assert(!created_shape.valid() && !created_container.valid());
    micropixel::scene_active = false;
}

void PendingCreationPublishesOneAtomicKeyframe() {
    micropixel::scene_active = true;
    micropixel::scene_storage.Reset({.logical_width = 8U, .logical_height = 8U});
    TestContainer scene{&micropixel::scene_storage, 0U, 0U};

    micropixel::scene_storage.BeginFrame();
    auto page = scene.CreateContainer({.translation = {2, 3}}).value();
    auto shape = page.CreateShape({1, 1, 2, 2}, micropixel::Color::White()).value();
    assert(page.valid() && shape.valid());
    assert(micropixel::EncodeAndSubmit(micropixel::scene_storage, true) == MICROPIXEL_STATUS_OK);
    micropixel::scene_storage.AcceptFrame();

    const auto header = SubmittedHeader();
    assert(header.kind == MICROPIXEL_GRAPHICS_SCENE_KEYFRAME);
    assert(header.container_count == 1U && header.node_count == 1U);
    assert(page.valid() && shape.valid());

    assert(page.Destroy().has_value());
    assert(!page.valid() && !shape.valid());
    assert(micropixel::EncodeAndSubmit(micropixel::scene_storage, true) == MICROPIXEL_STATUS_OK);
    micropixel::scene_storage.AcceptFrame();
    const auto destroyed_header = SubmittedHeader();
    assert(destroyed_header.kind == MICROPIXEL_GRAPHICS_SCENE_KEYFRAME);
    assert(destroyed_header.container_count == 0U && destroyed_header.node_count == 0U);
    micropixel::scene_active = false;
}

}  // namespace

void DirectPropertiesWaitForPresentAndRetry() {
    micropixel::Application app;
    auto renderer = app.renderer();
    auto scene = renderer.CreateScene().value();
    auto shape = scene.CreateShape({0, 0, 1, 1}, micropixel::Color::White()).value();
    assert(renderer.Present(scene).has_value());
    const auto previous = submitted_scene;
    shape.SetRect({2, 0, 1, 1});
    shape.SetColor(micropixel::Color::Green());
    assert(submitted_scene == previous);  // setters never cross the ABI
    submit_status = MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    assert(!renderer.Present(scene).has_value());
    assert(submitted_scene == previous);
    assert(shape.valid());
    submit_status = MICROPIXEL_STATUS_OK;
    assert(renderer.Present(scene).has_value());
    assert(submitted_scene != previous);
    assert(SubmittedHeader().kind == MICROPIXEL_GRAPHICS_SCENE_PATCH);
    micropixel_graphics_scene_rect_record_t rect{};
    std::memcpy(&rect, submitted_scene.data() + sizeof(micropixel_graphics_scene_header_t), sizeof(rect));
    assert(rect.x == 2);
    shape.Destroy();
    assert(!shape.valid());
    submit_status = MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    assert(!renderer.Present(scene).has_value());
    assert(!shape.valid());  // destruction is not undone on failure
    submit_status = MICROPIXEL_STATUS_OK;
    assert(renderer.Present(scene).has_value());
    assert(SubmittedHeader().node_count == 0U);
}

void ScenesSwitchWithKeyframesAndInvalidateHandles() {
    micropixel::Application app;
    auto renderer = app.renderer();
    micropixel::ShapeNode stale;
    {
        auto first = renderer.CreateScene().value();
        auto second = renderer.CreateScene().value();
        stale = first.CreateShape({1, 0, 1, 1}, micropixel::Color::White()).value();
        auto other = second.CreateShape({2, 0, 1, 1}, micropixel::Color::Green()).value();
        assert(renderer.Present(first));
        const auto generation = SubmittedHeader().generation;
        assert(renderer.Present(second));
        assert(SubmittedHeader().kind == MICROPIXEL_GRAPHICS_SCENE_KEYFRAME);
        assert(SubmittedHeader().generation != generation);
        assert(renderer.Present(first));
        assert(SubmittedHeader().kind == MICROPIXEL_GRAPHICS_SCENE_KEYFRAME);
        assert(other.valid() && stale.valid());
    }
    assert(!stale.valid());
    auto replacement = renderer.CreateScene().value();
    auto live = replacement.CreateShape({0, 0, 1, 1}, micropixel::Color::White()).value();
    assert(live.valid() && !stale.valid());
}

void FactoryFailurePreservesSceneAndReleasedBudgets() {
    using namespace micropixel;
    Application app;
    auto renderer = app.renderer();
    auto scene = renderer.CreateScene().value();
    auto parent = scene.CreateContainer().value();
    const auto stale = parent;
    parent.Destroy().value();
    auto stale_copy = stale;
    assert(stale_copy.CreateShape({0, 0, 1, 1}, Color::White()).error().code() == ErrorCode::kInvalidState);
    assert(!scene.CreateLabel({}, nullptr, Color::White()));
    assert(!scene.CreateLabel({}, "invalid font", Color::White(), static_cast<SystemFont>(0)));
    assert(!scene.CreateSprite(Texture{}, {}, {}));
    assert(scene.node_count() == 0);
    // No count cap: well past the old 256-node limit, creation only stops at
    // the shared uint16 item space (nodes + batch instances) or on OOM.
    ShapeNode first;
    for (uint32_t i = 0; i < 1000U; ++i) {
        auto created = scene.CreateShape({0, 0, 1, 1}, Color::White());
        assert(created);
        if (i == 0) first = created.value();
    }
    assert(scene.node_count() == 1000U);
    assert(renderer.Present(scene));
    first.Destroy();
    assert(scene.CreateShape({0, 0, 1, 1}, Color::White()));
    auto batch_scene = renderer.CreateScene().value();
    assert(batch_scene.CreateSpriteBatch(0).error().code() == ErrorCode::kInvalidArgument);
    constexpr uint16_t kLargeBatch = micropixel::runtime::limits::kMaxSceneItems - 1U;  // node + instances
    auto batch = batch_scene.CreateSpriteBatch(kLargeBatch).value();
    assert(batch_scene.CreateSpriteBatch(1).error().code() == ErrorCode::kResourceExhausted);
    assert(batch_scene.CreateShape({0, 0, 1, 1}, Color::White()).error().code() == ErrorCode::kResourceExhausted);
    assert(batch_scene.node_count() == 1);
    batch.Destroy();
    assert(batch_scene.CreateSpriteBatch(kLargeBatch));
    runtime::SceneArray<uint32_t> storage;
    assert(storage.Reserve(1));
    storage.push_back(42);
    const auto* original = storage.data();
    assert(!storage.Reserve(static_cast<size_t>(-1)));
    assert(storage.data() == original && storage.size() == 1 && storage[0] == 42);
}

void LabelTextLivesInAGrowingArena() {
    using namespace micropixel;
    Application app;
    auto renderer = app.renderer();
    auto scene = renderer.CreateScene().value();
    auto label = scene.CreateLabel({1, 1}, "a", Color::White()).value();
    assert(renderer.Present(scene).has_value());
    // Text of the single TEXT record in the last submitted message.
    const auto submitted_text = [] {
        size_t offset = sizeof(micropixel_graphics_scene_header_t);
        while (offset < submitted_scene.size()) {
            micropixel_graphics_scene_record_header_t record{};
            std::memcpy(&record, submitted_scene.data() + offset, sizeof(record));
            if (record.opcode == MICROPIXEL_GRAPHICS_SCENE_OP_TEXT) {
                micropixel_graphics_scene_text_record_t text{};
                std::memcpy(&text, submitted_scene.data() + offset, sizeof(text));
                return std::string(reinterpret_cast<const char*>(submitted_scene.data() + offset + sizeof(text)),
                                   text.text_length);
            }
            offset += record.size;
        }
        return std::string("<no text record>");
    };
    // Runs of varying length force appends, compaction and arena growth; every
    // patch must still carry exactly the current text.
    std::string text;
    for (uint32_t round = 1U; round <= 300U; ++round) {
        text.assign(1U + (round * 37U) % 900U, static_cast<char>('a' + round % 26));
        label.SetText(text.c_str());
        assert(renderer.Present(scene).has_value());
        assert(SubmittedHeader().kind == MICROPIXEL_GRAPHICS_SCENE_PATCH && submitted_text() == text);
    }
    // The Host text policy still bounds one run.
    const std::string longest(micropixel::runtime::limits::kMaxTextBytes, 'z');
    label.SetText(longest.c_str());
    assert(renderer.Present(scene).has_value() && submitted_text() == longest);
    const std::string too_long(micropixel::runtime::limits::kMaxTextBytes + 1U, 'z');
    assert(scene.CreateLabel({}, too_long.c_str(), Color::White()).error().code() == ErrorCode::kInvalidArgument);
    // A second label and a destroyed one keep their runs independent.
    auto other = scene.CreateLabel({2, 2}, "other", Color::White()).value();
    assert(renderer.Present(scene).has_value());
    label.Destroy();
    other.SetText("kept");
    assert(renderer.Present(scene).has_value());
    assert(SubmittedHeader().node_count == 1U && submitted_text() == "kept");
}

void DynamicTextureReferencesAdvanceOnlyWithAcceptedFrames() {
    using namespace micropixel;
    SceneState state;
    assert(state.Reset({.logical_width = 8, .logical_height = 8}));
    const auto id = state.AllocateNode({}).value();
    ConfigureShape(state, id, 0);
    auto& node = state.nodes[id];
    node.kind = SceneNodeKind::kSprite;
    node.dirty = FullMask(node.kind);
    node.texture_handle = 42;
    node.texture_logical_width = node.texture_logical_height = 1;
    node.texture_physical_width = node.texture_physical_height = 1;
    node.source = {0, 0, 1, 1};
    assert(runtime::RegisterDynamicTexture(42, MICROPIXEL_PIXEL_FORMAT_RGB565));
    Present(state);
    const auto prior = submitted_scene;
    const auto prior_revision = state.texture_revision;
    runtime::FindDynamicTexture(42)->snapshot = 84;
    ++runtime::texture_revision;
    submit_status = MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    assert(EncodeAndSubmit(state, false) == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    assert(submitted_scene == prior && state.texture_revision == prior_revision);
    submit_status = MICROPIXEL_STATUS_OK;
    assert(EncodeAndSubmit(state, false) == MICROPIXEL_STATUS_OK);
    assert(SubmittedHeader().kind == MICROPIXEL_GRAPHICS_SCENE_KEYFRAME);
    const size_t offset =
        sizeof(micropixel_graphics_scene_header_t) + sizeof(micropixel_graphics_scene_background_record_t);
    micropixel_graphics_scene_texture_record_t record{};
    std::memcpy(&record, submitted_scene.data() + offset, sizeof(record));
    assert(record.texture_handle == 84);
    assert(node.texture_handle == 42 && state.texture_revision == runtime::texture_revision);
    runtime::ForgetDynamicTexture(42);
}

int main() {
    LabelTextLivesInAGrowingArena();
    DynamicTextureReferencesAdvanceOnlyWithAcceptedFrames();
    FactoryFailurePreservesSceneAndReleasedBudgets();
    DestroyReusesSlotsAndPreservesDrawOrder();
    DestroyedHandleStaysInvalidAcrossFrames();
    SubmitFailureKeepsDestroyedNode();
    RepeatedDestroyAndReuseDoesNotExhaustThePool();
    StorageGrowsOnDemandAndResetInvalidatesHandles();
    ContainerDestroyCascadesPermanently();
    SpriteBatchDestroyCompactsInstances();
    NestedContainerCoordinatesAreLocalToTheirParent();
    CreationFailureAndDestructionDoNotAliasReusedSlots();
    PendingCreationPublishesOneAtomicKeyframe();
    DirectPropertiesWaitForPresentAndRetry();
    ScenesSwitchWithKeyframesAndInvalidateHandles();
    return 0;
}
