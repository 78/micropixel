#ifndef MICROPIXEL_SDK_MESH_RENDERER_HPP
#define MICROPIXEL_SDK_MESH_RENDERER_HPP

#include <stdint.h>

#include <span>

#include "sdk/geometry.hpp"
#include "sdk/graphics.hpp"

namespace micropixel {

// PS1-style polygon front end over the Host raster kernels (Graphics 1.6
// TRIANGLE / QUAD records).
//
// The App owns meshes (vertices + textured, per-corner lit faces) and their
// transforms; the MeshRenderer runs the per-frame geometry: view transform,
// near-plane clipping, back-face culling, distance lighting, subdivision of
// large near faces (to hide the affine texture warp), exact clipping to a
// scissor rectangle, and an ordering table that sorts polygons far to near
// without a Z-buffer. Flush() appends the sorted records to a RasterDrawList;
// the Host fills every pixel.
//
// What it deliberately does not do: per-pixel depth (interpenetrating
// geometry resolves by polygon depth only) and perspective-correct texturing.
// Scenes that keep visibility under control (rooms and portals, fixed-camera
// sets, tracks) fit; open worlds with heavy overdraw do not.
//
// Coordinates: world and view space are x right, y up, z forward (into the
// screen). Faces list their corners counter-clockwise as seen from the front.
// A camera at the origin with yaw 0 and pitch 0 looks along +z; yaw turns
// towards +x, pitch lifts the view towards +y.

struct Vec3 final {
    float x{};
    float y{};
    float z{};

    [[nodiscard]] constexpr Vec3 operator+(Vec3 other) const { return {x + other.x, y + other.y, z + other.z}; }
    [[nodiscard]] constexpr Vec3 operator-(Vec3 other) const { return {x - other.x, y - other.y, z - other.z}; }
    [[nodiscard]] constexpr Vec3 operator*(float scale) const { return {x * scale, y * scale, z * scale}; }
    [[nodiscard]] constexpr float Dot(Vec3 other) const { return x * other.x + y * other.y + z * other.z; }
    [[nodiscard]] constexpr Vec3 Cross(Vec3 o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    [[nodiscard]] float Length() const;
};

// Rigid transform: world = rotation * local + translation. Rotation rows are
// `m[row][column]`. Compose parent and child with operator*: (parent * child)
// applies the child first.
struct Transform3 final {
    float m[3][3]{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
    Vec3 t{};

    [[nodiscard]] static constexpr Transform3 Identity() { return {}; }
    [[nodiscard]] static constexpr Transform3 Translation(Vec3 offset) {
        Transform3 result{};
        result.t = offset;
        return result;
    }
    // Rotations by `radians` about one axis. RotationY(+a) turns +z towards
    // +x (camera yaw), RotationX(+a) turns +z towards +y (pitch up),
    // RotationZ(+a) turns +x towards +y (roll).
    [[nodiscard]] static Transform3 RotationX(float radians);
    [[nodiscard]] static Transform3 RotationY(float radians);
    [[nodiscard]] static Transform3 RotationZ(float radians);
    [[nodiscard]] static Transform3 Uniform(Vec3 translation, float yaw, float pitch = 0.0F, float roll = 0.0F);

    [[nodiscard]] constexpr Vec3 Apply(Vec3 p) const {
        return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + t.x,
                m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + t.y,
                m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + t.z};
    }
    [[nodiscard]] constexpr Vec3 Rotate(Vec3 p) const {
        return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z, m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z,
                m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z};
    }
    [[nodiscard]] constexpr Transform3 operator*(const Transform3& child) const {
        Transform3 result{};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                result.m[row][column] =
                    m[row][0] * child.m[0][column] + m[row][1] * child.m[1][column] + m[row][2] * child.m[2][column];
            }
        }
        result.t = Apply(child.t);
        return result;
    }
};

struct MeshCamera final {
    Vec3 position{};
    float yaw{};    // radians about +y; 0 looks along +z, positive turns towards +x
    float pitch{};  // radians; positive looks up
    // Pixels per unit at distance 1. FocalLength() derives it from a
    // horizontal field of view.
    float focal_length{240.0F};
    // View-space depth below which geometry is clipped. Must be positive.
    float near{0.1F};

    [[nodiscard]] static float FocalLength(float horizontal_fov_radians, int view_width);
};

struct MeshVertex final {
    float x{};
    float y{};
    float z{};
};

enum MeshFaceFlag : uint8_t {
    // Texel index 0 is not drawn.
    kMeshFaceTransparent = 1U << 0U,
    // No texture: palette entry u[0] at the interpolated light.
    kMeshFaceFlatColor = 1U << 1U,
    // Drawn from both sides (no back-face culling).
    kMeshFaceDoubleSided = 1U << 2U,
};

// A triangle (vertex[3] == kNoVertex) or convex quad. Corners are listed
// counter-clockwise from the front. u/v are texel coordinates of a kRowMajor
// power-of-two texture; a face may span fewer than 256 texels on each axis
// (the wire format wraps on 256). `brightness` is the corner's light before
// distance darkening, 0..255.
struct MeshFace final {
    static constexpr uint16_t kNoVertex = 0xFFFFU;

    uint16_t vertex[4]{0U, 0U, 0U, kNoVertex};
    uint16_t u[4]{};
    uint16_t v[4]{};
    uint8_t brightness[4]{255U, 255U, 255U, 255U};
    uint8_t texture_slot{};
    uint8_t flags{};

    [[nodiscard]] constexpr uint32_t corners() const { return vertex[3] == kNoVertex ? 3U : 4U; }
};

struct Mesh final {
    std::span<const MeshVertex> vertices;
    std::span<const MeshFace> faces;
};

// Distance darkening: light = brightness / 255 * (levels - 1) * attenuation,
// attenuation 1 up to full_distance and falling linearly to 0 at dark_distance,
// then clamped to at least `minimum`. The lit palette uploaded through
// RasterResources needs `levels` rows.
struct MeshLighting final {
    uint8_t levels{16U};
    uint8_t minimum{1U};
    float full_distance{4.0F};
    float dark_distance{28.0F};
};

struct MeshRendererConfig final {
    // Target buffer size in pixels.
    int width{};
    int height{};
    MeshLighting lighting{};
    // Depth of the farthest ordering bucket; polygons beyond it share the last
    // bucket.
    float far{48.0F};
    // A face whose farthest corner is deeper than its nearest by more than
    // this ratio, and which covers more than `subdivide_min_pixels` on
    // screen, is split into four (recursively, `subdivide_levels` deep). Lower
    // ratios hide more affine warp at the cost of polygons.
    float subdivide_depth_ratio{1.6F};
    int subdivide_min_pixels{120};
    uint8_t subdivide_levels{2U};
};

// One sorted polygon awaiting Flush(); the App provides the pool.
struct MeshPolygonSlot final {
    RasterVertex corners[4];
    uint16_t next{};
    uint8_t count{};
    uint8_t texture_slot{};
    uint8_t flags{};
};

struct MeshSubmitOptions final {
    // Ordering group: groups flush in ascending order, so a farther room goes
    // in a lower group than the room the camera stands in.
    uint8_t group{};
    // Added to the depth key before bucketing: negative draws later (nearer).
    // Characters standing on large floor polygons use a small negative bias.
    float depth_bias{};
    // Polygons are clipped exactly to this rectangle (buffer pixels); an empty
    // rectangle means the whole view. Portals set their screen bounds here.
    Rect scissor{};
};

class MeshRenderer final {
   public:
    static constexpr uint32_t kBuckets = 512U;
    static constexpr uint32_t kMaxGroups = 8U;
    static constexpr uint32_t kMaxMeshVertices = 1024U;

    struct Storage final {
        std::span<MeshPolygonSlot> polygons;
        // group_count * kBuckets entries.
        std::span<uint16_t> buckets;
    };

    struct Stats final {
        uint32_t faces{};           // faces submitted
        uint32_t culled{};          // back-facing, behind the camera or outside the scissor
        uint32_t clipped{};         // faces that touched the near plane or scissor
        uint32_t subdivided{};      // faces split for the affine warp
        uint32_t polygons{};        // records queued
        uint32_t dropped{};         // polygons lost to a full pool
        uint32_t pixel_estimate{};  // screen-space area of the queued polygons
    };

    MeshRenderer() = default;
    MeshRenderer(const MeshRenderer&) = delete;
    MeshRenderer& operator=(const MeshRenderer&) = delete;

    // `storage.buckets` must hold at least `groups * kBuckets` entries
    // (groups <= kMaxGroups). False leaves the previous configuration.
    [[nodiscard]] bool Initialize(const MeshRendererConfig& config, Storage storage, uint8_t groups = 1U);
    [[nodiscard]] const MeshRendererConfig& config() const { return config_; }

    // Starts a frame: empties the ordering table and fixes the camera.
    void Begin(const MeshCamera& camera);
    [[nodiscard]] const MeshCamera& camera() const { return camera_; }
    // Queues every visible face of `mesh` placed by `transform`. False when the
    // mesh has more than kMaxMeshVertices vertices or the group is out of
    // range; nothing is queued then.
    [[nodiscard]] bool Submit(const Mesh& mesh, const Transform3& transform, const MeshSubmitOptions& options = {});
    // Appends the queued polygons far to near, group by group. False when a
    // record could not be encoded.
    [[nodiscard]] bool Flush(RasterDrawList& list);
    [[nodiscard]] const Stats& stats() const { return stats_; }

    // World point to view space / screen with the camera of the last Begin().
    [[nodiscard]] Vec3 ToView(Vec3 world) const;
    // False when the point is not in front of the near plane.
    [[nodiscard]] bool Project(Vec3 world, float& x_out, float& y_out, float& depth_out) const;

   private:
    struct ClipVertex final {
        Vec3 p{};
        float u{};
        float v{};
        float light{};
    };
    struct ScreenVertex final {
        float x{};
        float y{};
        float u{};
        float v{};
        float light{};
    };
    static constexpr uint32_t kMaxClipVertices = 12U;

    void SubmitFace(const MeshFace& face, const MeshSubmitOptions& options, Rect scissor);
    void EmitViewPolygon(const ClipVertex* vertices, uint32_t count, const MeshFace& face,
                         const MeshSubmitOptions& options, Rect scissor, float depth, uint8_t level);
    void EmitScreenPolygon(ScreenVertex* vertices, uint32_t count, const MeshFace& face, uint8_t group, float depth,
                           Rect scissor);
    void Queue(const ScreenVertex* vertices, uint32_t count, const MeshFace& face, uint8_t group, float depth);
    [[nodiscard]] float LightFor(float brightness, float depth) const;

    MeshRendererConfig config_{};
    Storage storage_{};
    uint8_t groups_{};
    MeshCamera camera_{};
    Transform3 world_to_view_{};
    float center_x_{};
    float center_y_{};
    float bucket_scale_{};
    uint32_t used_{};
    Stats stats_{};
    Vec3 view_vertices_[kMaxMeshVertices]{};
};

// Convenience storage for a MeshRenderer, sized at compile time and placed
// by the App in static memory (a slot is 44 bytes).
template <uint32_t kPolygons, uint32_t kGroups = 1U>
struct MeshRendererPool final {
    static_assert(kPolygons > 0U && kPolygons < 0xFFFFU && kGroups >= 1U && kGroups <= MeshRenderer::kMaxGroups);
    MeshPolygonSlot polygons[kPolygons];
    uint16_t buckets[kGroups * MeshRenderer::kBuckets];

    [[nodiscard]] MeshRenderer::Storage storage() { return {polygons, buckets}; }
    [[nodiscard]] static constexpr uint8_t groups() { return static_cast<uint8_t>(kGroups); }
};

}  // namespace micropixel

#endif
