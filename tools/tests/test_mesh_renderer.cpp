// Host-side checks for the Guest SDK MeshRenderer: transforms, projection,
// culling, near/scissor clipping, ordering table and record emission. The
// renderer is built from guest/runtime/mesh_renderer.cpp; the RasterDrawList
// polygon methods are replaced by recording stubs below.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "sdk/mesh_renderer.hpp"

using namespace micropixel;

namespace {

struct Record final {
    uint8_t count{};
    uint8_t slot{};
    bool flat{};
    bool transparent{};
    RasterVertex corners[4]{};
};

std::vector<Record> g_records;

void Check(bool condition, const char* message, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, message);
        std::exit(1);
    }
}
#define CHECK(condition, message) Check((condition), (message), __LINE__)

bool Near(float a, float b, float tolerance = 1e-3F) { return std::fabs(a - b) <= tolerance; }

// The SDK never exposes a RasterDrawList constructor outside HostSurface; the
// test only needs an object for the stubs below to be called on.
struct ListStorage final {
    alignas(RasterDrawList) unsigned char bytes[sizeof(RasterDrawList)]{};
    RasterDrawList& list() { return *std::launder(reinterpret_cast<RasterDrawList*>(bytes)); }
};

constexpr int kWidth = 480;
constexpr int kHeight = 480;

MeshRendererConfig Config() {
    MeshRendererConfig config{};
    config.width = kWidth;
    config.height = kHeight;
    config.lighting.levels = 16U;
    config.lighting.minimum = 1U;
    config.lighting.full_distance = 4.0F;
    config.lighting.dark_distance = 28.0F;
    config.far = 48.0F;
    return config;
}

MeshCamera Camera() {
    MeshCamera camera{};
    camera.focal_length = 240.0F;
    camera.near = 0.1F;
    return camera;
}

// Quad facing -z (towards a camera at the origin looking along +z), listed
// counter-clockwise as seen from the camera: y up, x right.
struct QuadMesh final {
    MeshVertex vertices[4];
    MeshFace face{};
    Mesh mesh() const { return {std::span<const MeshVertex>(vertices, 4U), std::span<const MeshFace>(&face, 1U)}; }
};

QuadMesh FacingQuad(float x0, float x1, float y0, float y1, float z) {
    QuadMesh q{};
    q.vertices[0] = {x0, y0, z};
    q.vertices[1] = {x1, y0, z};
    q.vertices[2] = {x1, y1, z};
    q.vertices[3] = {x0, y1, z};
    q.face.vertex[0] = 0U;
    q.face.vertex[1] = 1U;
    q.face.vertex[2] = 2U;
    q.face.vertex[3] = 3U;
    const uint16_t us[4] = {0U, 64U, 64U, 0U};
    const uint16_t vs[4] = {64U, 64U, 0U, 0U};
    for (int c = 0; c < 4; ++c) {
        q.face.u[c] = us[c];
        q.face.v[c] = vs[c];
        q.face.brightness[c] = 255U;
    }
    q.face.texture_slot = 3U;
    return q;
}

// Horizontal quad (floor) at height y spanning x0..x1, z0..z1, facing up.
QuadMesh FloorQuad(float x0, float x1, float z0, float z1, float y) {
    QuadMesh q{};
    q.vertices[0] = {x0, y, z0};
    q.vertices[1] = {x1, y, z0};
    q.vertices[2] = {x1, y, z1};
    q.vertices[3] = {x0, y, z1};
    q.face.vertex[0] = 0U;
    q.face.vertex[1] = 1U;
    q.face.vertex[2] = 2U;
    q.face.vertex[3] = 3U;
    for (int c = 0; c < 4; ++c) q.face.brightness[c] = 255U;
    q.face.texture_slot = 1U;
    return q;
}

void TestTransforms() {
    const Transform3 yaw = Transform3::RotationY(1.5707963F);
    Vec3 p = yaw.Rotate({0.0F, 0.0F, 1.0F});
    CHECK(Near(p.x, 1.0F) && Near(p.y, 0.0F) && Near(p.z, 0.0F), "RotationY(+90) turns +z to +x");
    const Transform3 pitch = Transform3::RotationX(1.5707963F);
    p = pitch.Rotate({0.0F, 0.0F, 1.0F});
    CHECK(Near(p.x, 0.0F) && Near(p.y, 1.0F) && Near(p.z, 0.0F), "RotationX(+90) turns +z to +y");
    const Transform3 roll = Transform3::RotationZ(1.5707963F);
    p = roll.Rotate({1.0F, 0.0F, 0.0F});
    CHECK(Near(p.x, 0.0F) && Near(p.y, 1.0F) && Near(p.z, 0.0F), "RotationZ(+90) turns +x to +y");

    const Transform3 parent = Transform3::Translation({1.0F, 2.0F, 3.0F}) * Transform3::RotationY(1.5707963F);
    const Transform3 child = Transform3::Translation({0.0F, 0.0F, 1.0F});
    const Transform3 composed = parent * child;
    p = composed.Apply({0.0F, 0.0F, 0.0F});
    CHECK(Near(p.x, 2.0F) && Near(p.y, 2.0F) && Near(p.z, 3.0F), "parent * child applies the child first");

    const Transform3 uniform = Transform3::Uniform({5.0F, 0.0F, 0.0F}, 3.1415926F);
    p = uniform.Apply({0.0F, 0.0F, 1.0F});
    CHECK(Near(p.x, 5.0F, 1e-2F) && Near(p.z, -1.0F, 1e-2F), "Uniform yaw of 180 degrees mirrors z");

    CHECK(Near(Vec3{3.0F, 4.0F, 0.0F}.Length(), 5.0F), "Vec3::Length");
    const float focal = MeshCamera::FocalLength(1.5707963F, 480);
    CHECK(Near(focal, 240.0F, 0.5F), "90 degree horizontal FOV on 480 pixels is focal length 240");
}

void TestProjection() {
    MeshRendererPool<64> pool;
    MeshRenderer renderer;
    CHECK(renderer.Initialize(Config(), pool.storage(), 1U), "Initialize");
    renderer.Begin(Camera());
    float x{}, y{}, depth{};
    CHECK(renderer.Project({0.0F, 0.0F, 2.0F}, x, y, depth), "point ahead projects");
    CHECK(Near(x, 240.0F) && Near(y, 240.0F) && Near(depth, 2.0F), "centre projects to the buffer centre");
    CHECK(renderer.Project({1.0F, 1.0F, 2.0F}, x, y, depth), "offset point projects");
    CHECK(Near(x, 360.0F) && Near(y, 120.0F), "+x goes right, +y goes up on screen");
    CHECK(!renderer.Project({0.0F, 0.0F, 0.05F}, x, y, depth), "points before the near plane do not project");

    MeshCamera turned = Camera();
    turned.yaw = 1.5707963F;
    turned.position = {0.0F, 0.0F, 0.0F};
    renderer.Begin(turned);
    CHECK(renderer.Project({2.0F, 0.0F, 0.0F}, x, y, depth), "yawed camera sees +x");
    CHECK(Near(x, 240.0F, 0.05F) && Near(depth, 2.0F, 1e-3F), "yaw +90 looks along +x");

    MeshCamera moved = Camera();
    moved.position = {0.0F, 1.0F, -3.0F};
    renderer.Begin(moved);
    CHECK(renderer.Project({0.0F, 1.0F, 0.0F}, x, y, depth) && Near(depth, 3.0F), "camera position offsets depth");
}

void TestSubmitAndCull() {
    MeshRendererPool<64> pool;
    MeshRenderer renderer;
    CHECK(renderer.Initialize(Config(), pool.storage(), 1U), "Initialize");
    renderer.Begin(Camera());

    // Front-facing quad two units ahead: one polygon, no clipping.
    const QuadMesh quad = FacingQuad(-1.0F, 1.0F, -1.0F, 1.0F, 2.0F);
    CHECK(renderer.Submit(quad.mesh(), Transform3::Identity()), "Submit");
    const MeshRenderer::Stats& stats = renderer.stats();
    CHECK(stats.faces == 1U && stats.culled == 0U && stats.polygons == 1U && stats.clipped == 0U,
          "facing quad queues exactly one polygon");
    const MeshPolygonSlot& slot = pool.polygons[0];
    CHECK(slot.count == 4U && slot.texture_slot == 3U, "slot carries corner count and texture");
    // Corners: (-1,-1) -> screen (120, 360) etc. 12.4 fixed point.
    CHECK(slot.corners[0].x == 120 * 16 && slot.corners[0].y == 360 * 16, "corner 0 position");
    CHECK(slot.corners[2].x == 360 * 16 && slot.corners[2].y == 120 * 16, "corner 2 position");
    CHECK(slot.corners[1].u == 64U * 256U && slot.corners[1].v == 64U * 256U, "texels are 8.8");
    CHECK(slot.corners[0].light == 15U, "full brightness inside full_distance is the top level");
    CHECK(stats.pixel_estimate > 57000U && stats.pixel_estimate < 58000U, "pixel estimate is the screen area");

    // The same quad seen from behind is culled unless double sided.
    MeshCamera behind = Camera();
    behind.position = {0.0F, 0.0F, 4.0F};
    behind.yaw = 3.1415926F;
    renderer.Begin(behind);
    CHECK(renderer.Submit(quad.mesh(), Transform3::Identity()), "Submit from behind");
    CHECK(renderer.stats().culled == 1U && renderer.stats().polygons == 0U, "back face culled");
    QuadMesh both = quad;
    both.face.flags = kMeshFaceDoubleSided;
    CHECK(renderer.Submit(both.mesh(), Transform3::Identity()), "Submit double sided");
    CHECK(renderer.stats().polygons == 1U, "double sided face survives from behind");

    // Entirely behind the camera.
    renderer.Begin(Camera());
    CHECK(renderer.Submit(quad.mesh(), Transform3::Translation({0.0F, 0.0F, -5.0F})), "Submit behind");
    CHECK(renderer.stats().culled == 1U && renderer.stats().polygons == 0U, "faces behind the camera are culled");

    // Off to the side, outside the view.
    renderer.Begin(Camera());
    CHECK(renderer.Submit(quad.mesh(), Transform3::Translation({10.0F, 0.0F, 0.0F})), "Submit off screen");
    CHECK(renderer.stats().culled == 1U && renderer.stats().polygons == 0U, "off-screen faces are culled");

    // Invalid vertex index and oversized group are rejected safely.
    QuadMesh bad = quad;
    bad.face.vertex[2] = 9U;
    renderer.Begin(Camera());
    CHECK(renderer.Submit(bad.mesh(), Transform3::Identity()), "Submit bad index");
    CHECK(renderer.stats().culled == 1U && renderer.stats().polygons == 0U, "faces with bad indices are skipped");
    MeshSubmitOptions options{};
    options.group = 1U;
    CHECK(!renderer.Submit(quad.mesh(), Transform3::Identity(), options), "group out of range is rejected");
}

void TestLighting() {
    MeshRendererPool<64> pool;
    MeshRenderer renderer;
    MeshRendererConfig config = Config();
    config.subdivide_levels = 0U;
    CHECK(renderer.Initialize(config, pool.storage(), 1U), "Initialize");
    renderer.Begin(Camera());
    // Half way between full (4) and dark (28): attenuation 0.5 -> level 7.5 -> 8 after rounding.
    QuadMesh quad = FacingQuad(-1.0F, 1.0F, -1.0F, 1.0F, 16.0F);
    quad.face.brightness[0] = 128U;  // 128/255 * 15 * 0.5 = 3.76 -> 4
    CHECK(renderer.Submit(quad.mesh(), Transform3::Identity()), "Submit lit quad");
    CHECK(renderer.stats().polygons == 1U, "queued");
    CHECK(pool.polygons[0].corners[1].light == 8U, "distance attenuation halves the light");
    CHECK(pool.polygons[0].corners[0].light == 4U, "corner brightness scales the light");
    renderer.Begin(Camera());
    quad = FacingQuad(-1.0F, 1.0F, -1.0F, 1.0F, 40.0F);
    CHECK(renderer.Submit(quad.mesh(), Transform3::Identity()), "Submit far quad");
    CHECK(pool.polygons[0].corners[0].light == 1U, "beyond dark_distance the minimum level applies");
}

void TestNearClip() {
    MeshRendererPool<64> pool;
    MeshRenderer renderer;
    CHECK(renderer.Initialize(Config(), pool.storage(), 1U), "Initialize");
    renderer.Begin(Camera());
    // Floor quad under the camera running from behind it to ahead of it.
    const QuadMesh floor = FloorQuad(-2.0F, 2.0F, -2.0F, 6.0F, -1.0F);
    CHECK(renderer.Submit(floor.mesh(), Transform3::Identity()), "Submit floor");
    const MeshRenderer::Stats& stats = renderer.stats();
    CHECK(stats.clipped >= 1U && stats.polygons >= 1U && stats.dropped == 0U,
          "floor crossing the near plane is clipped");
    // All queued corners must sit inside the buffer (clipped to the view) and
    // no vertex may have gone through the camera.
    for (uint32_t i = 0U; i < stats.polygons; ++i) {
        const MeshPolygonSlot& slot = pool.polygons[i];
        for (uint32_t c = 0U; c < slot.count; ++c) {
            CHECK(slot.corners[c].x >= 0 && slot.corners[c].x <= kWidth * 16, "clipped x inside the buffer");
            CHECK(slot.corners[c].y >= 0 && slot.corners[c].y <= kHeight * 16, "clipped y inside the buffer");
        }
    }
}

void TestScissor() {
    MeshRendererPool<64> pool;
    MeshRenderer renderer;
    MeshRendererConfig config = Config();
    config.subdivide_levels = 0U;
    CHECK(renderer.Initialize(config, pool.storage(), 1U), "Initialize");
    renderer.Begin(Camera());
    const QuadMesh quad = FacingQuad(-1.0F, 1.0F, -1.0F, 1.0F, 2.0F);  // screen 120..360
    MeshSubmitOptions options{};
    options.scissor = Rect{200, 100, 100, 300};  // x 200..300, y 100..400
    CHECK(renderer.Submit(quad.mesh(), Transform3::Identity(), options), "Submit with scissor");
    const MeshRenderer::Stats& stats = renderer.stats();
    CHECK(stats.clipped == 1U && stats.polygons >= 1U, "quad crossing the scissor is clipped");
    uint32_t area = 0U;
    for (uint32_t i = 0U; i < stats.polygons; ++i) {
        const MeshPolygonSlot& slot = pool.polygons[i];
        for (uint32_t c = 0U; c < slot.count; ++c) {
            CHECK(slot.corners[c].x >= 200 * 16 && slot.corners[c].x <= 300 * 16, "x clipped to the scissor");
            CHECK(slot.corners[c].y >= 120 * 16 && slot.corners[c].y <= 360 * 16, "y keeps the quad's own bounds");
            // Texels stay inside the face's 0..64 range.
            CHECK(slot.corners[c].u <= 64U * 256U && slot.corners[c].v <= 64U * 256U, "texels interpolated");
        }
    }
    area = stats.pixel_estimate;
    CHECK(area > 23500U && area < 24500U, "clipped area is the 100x240 intersection");

    // Scissor completely outside the quad culls it without queueing.
    renderer.Begin(Camera());
    options.scissor = Rect{0, 0, 100, 100};
    CHECK(renderer.Submit(quad.mesh(), Transform3::Identity(), options), "Submit outside scissor");
    CHECK(renderer.stats().culled == 1U && renderer.stats().polygons == 0U, "faces outside the scissor are culled");
}

void TestSubdivision() {
    MeshRendererPool<256> pool;
    MeshRenderer renderer;
    MeshRendererConfig config = Config();
    config.subdivide_levels = 1U;
    CHECK(renderer.Initialize(config, pool.storage(), 1U), "Initialize");
    renderer.Begin(Camera());
    // Large floor from depth 1 to depth 8: depth ratio 8 > 1.6, large on screen.
    const QuadMesh floor = FloorQuad(-3.0F, 3.0F, 1.0F, 8.0F, -1.0F);
    CHECK(renderer.Submit(floor.mesh(), Transform3::Identity()), "Submit floor");
    CHECK(renderer.stats().subdivided == 1U, "one face subdivided at level 1");
    CHECK(renderer.stats().polygons >= 4U, "subdivision yields at least four polygons");

    config.subdivide_levels = 2U;
    CHECK(renderer.Initialize(config, pool.storage(), 1U), "Initialize");
    renderer.Begin(Camera());
    CHECK(renderer.Submit(floor.mesh(), Transform3::Identity()), "Submit floor");
    CHECK(renderer.stats().subdivided >= 2U, "two levels subdivide recursively");

    // A small distant face is left alone.
    renderer.Begin(Camera());
    const QuadMesh far = FloorQuad(-0.5F, 0.5F, 20.0F, 21.0F, -1.0F);
    CHECK(renderer.Submit(far.mesh(), Transform3::Identity()), "Submit far floor");
    CHECK(renderer.stats().subdivided == 0U && renderer.stats().polygons == 1U, "small faces are not split");
}

void TestOrderingAndFlush() {
    MeshRendererPool<64, 2> pool;
    MeshRenderer renderer;
    MeshRendererConfig config = Config();
    config.subdivide_levels = 0U;
    CHECK(renderer.Initialize(config, pool.storage(), pool.groups()), "Initialize");
    renderer.Begin(Camera());
    // Near quad first, far quad second, plus a group-0 quad that is nearer than
    // both but must flush first because groups flush in ascending order.
    const QuadMesh near = FacingQuad(-0.5F, 0.5F, -0.5F, 0.5F, 2.0F);
    const QuadMesh far = FacingQuad(-0.5F, 0.5F, -0.5F, 0.5F, 10.0F);
    const QuadMesh nearest = FacingQuad(-0.5F, 0.5F, -0.5F, 0.5F, 1.0F);
    MeshSubmitOptions group1{};
    group1.group = 1U;
    QuadMesh near_slot = near;
    near_slot.face.texture_slot = 2U;
    QuadMesh far_slot = far;
    far_slot.face.texture_slot = 10U;
    QuadMesh nearest_slot = nearest;
    nearest_slot.face.texture_slot = 1U;
    CHECK(renderer.Submit(near_slot.mesh(), Transform3::Identity(), group1), "Submit near");
    CHECK(renderer.Submit(far_slot.mesh(), Transform3::Identity(), group1), "Submit far");
    CHECK(renderer.Submit(nearest_slot.mesh(), Transform3::Identity()), "Submit nearest in group 0");
    // A flat coloured triangle in group 1, in between.
    QuadMesh flat = FacingQuad(-0.5F, 0.5F, -0.5F, 0.5F, 5.0F);
    flat.face.vertex[3] = MeshFace::kNoVertex;
    flat.face.flags = kMeshFaceFlatColor | kMeshFaceTransparent;
    flat.face.u[0] = 77U;
    CHECK(renderer.Submit(flat.mesh(), Transform3::Identity(), group1), "Submit flat triangle");
    CHECK(renderer.stats().polygons == 4U, "four polygons queued");

    g_records.clear();
    ListStorage storage;
    CHECK(renderer.Flush(storage.list()), "Flush");
    CHECK(g_records.size() == 4U, "Flush emits every polygon");
    CHECK(g_records[0].slot == 1U, "group 0 flushes first");
    CHECK(g_records[1].slot == 10U && !g_records[1].flat, "group 1: far quad first");
    CHECK(g_records[2].flat && g_records[2].count == 3U && g_records[2].slot == 77U && !g_records[2].transparent,
          "flat triangle carries its palette index");
    CHECK(g_records[3].slot == 2U, "group 1: near quad last");

    // Depth bias moves a polygon to a later bucket.
    renderer.Begin(Camera());
    MeshSubmitOptions biased{};
    biased.depth_bias = -5.0F;
    CHECK(renderer.Submit(far_slot.mesh(), Transform3::Identity(), biased), "Submit biased far");
    CHECK(renderer.Submit(near_slot.mesh(), Transform3::Identity()), "Submit near");
    g_records.clear();
    CHECK(renderer.Flush(storage.list()), "Flush");
    CHECK(g_records.size() == 2U && g_records[0].slot == 10U && g_records[1].slot == 2U,
          "a bias of -5 keeps depth 10 behind depth 2");
    renderer.Begin(Camera());
    biased.depth_bias = -9.0F;
    CHECK(renderer.Submit(far_slot.mesh(), Transform3::Identity(), biased), "Submit heavily biased far");
    CHECK(renderer.Submit(near_slot.mesh(), Transform3::Identity()), "Submit near");
    g_records.clear();
    CHECK(renderer.Flush(storage.list()), "Flush");
    CHECK(g_records[0].slot == 2U && g_records[1].slot == 10U, "a bias of -9 draws depth 10 after depth 2");

    // Begin() empties the table.
    renderer.Begin(Camera());
    g_records.clear();
    CHECK(renderer.Flush(storage.list()), "Flush empty");
    CHECK(g_records.empty(), "nothing queued after Begin");
}

void TestPoolExhaustion() {
    MeshRendererPool<2> pool;
    MeshRenderer renderer;
    MeshRendererConfig config = Config();
    config.subdivide_levels = 0U;
    CHECK(renderer.Initialize(config, pool.storage(), 1U), "Initialize");
    renderer.Begin(Camera());
    const QuadMesh quad = FacingQuad(-0.5F, 0.5F, -0.5F, 0.5F, 2.0F);
    for (int i = 0; i < 5; ++i) CHECK(renderer.Submit(quad.mesh(), Transform3::Identity()), "Submit");
    CHECK(renderer.stats().polygons == 2U && renderer.stats().dropped == 3U, "a full pool drops polygons");

    MeshRenderer bad;
    CHECK(!bad.Initialize(config, pool.storage(), 2U), "too few buckets for two groups");
    MeshRendererConfig zero = config;
    zero.width = 0;
    CHECK(!bad.Initialize(zero, pool.storage(), 1U), "zero width rejected");
}

void TestTexelWrap() {
    MeshRendererPool<8> pool;
    MeshRenderer renderer;
    MeshRendererConfig config = Config();
    config.subdivide_levels = 0U;
    CHECK(renderer.Initialize(config, pool.storage(), 1U), "Initialize");
    renderer.Begin(Camera());
    // Texels 512..576 (a tiled floor) must be shifted into the 8.8 range
    // without breaking their relative span.
    QuadMesh quad = FacingQuad(-1.0F, 1.0F, -1.0F, 1.0F, 2.0F);
    for (int c = 0; c < 4; ++c) quad.face.u[c] = static_cast<uint16_t>(quad.face.u[c] + 512U);
    CHECK(renderer.Submit(quad.mesh(), Transform3::Identity()), "Submit");
    CHECK(pool.polygons[0].corners[0].u == 0U && pool.polygons[0].corners[1].u == 64U * 256U,
          "texel coordinates are shifted by a multiple of 256");
}

}  // namespace

// Recording stubs for the RasterDrawList polygon methods.
namespace micropixel {

bool RasterDrawList::Triangle(const RasterVertex (&corners)[3], uint8_t texture_slot, bool transparent) {
    Record record{3U, texture_slot, false, transparent};
    for (int c = 0; c < 3; ++c) record.corners[c] = corners[c];
    g_records.push_back(record);
    return true;
}

bool RasterDrawList::Quad(const RasterVertex (&corners)[4], uint8_t texture_slot, bool transparent) {
    Record record{4U, texture_slot, false, transparent};
    for (int c = 0; c < 4; ++c) record.corners[c] = corners[c];
    g_records.push_back(record);
    return true;
}

bool RasterDrawList::FlatTriangle(const RasterVertex (&corners)[3], uint8_t color_index) {
    Record record{3U, color_index, true, false};
    for (int c = 0; c < 3; ++c) record.corners[c] = corners[c];
    g_records.push_back(record);
    return true;
}

bool RasterDrawList::FlatQuad(const RasterVertex (&corners)[4], uint8_t color_index) {
    Record record{4U, color_index, true, false};
    for (int c = 0; c < 4; ++c) record.corners[c] = corners[c];
    g_records.push_back(record);
    return true;
}

}  // namespace micropixel

int main() {
    TestTransforms();
    TestProjection();
    TestSubmitAndCull();
    TestLighting();
    TestNearClip();
    TestScissor();
    TestSubdivision();
    TestOrderingAndFlush();
    TestPoolExhaustion();
    TestTexelWrap();
    std::puts("mesh_renderer: ok");
    return 0;
}
