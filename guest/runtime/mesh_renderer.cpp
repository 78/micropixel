#include "sdk/mesh_renderer.hpp"

namespace micropixel {
namespace {

constexpr float kPi = 3.14159265358979F;
constexpr float kTwoPi = 6.28318530717959F;
constexpr float kHalfPi = 1.57079632679490F;
constexpr uint16_t kNoSlot = 0xFFFFU;

// Freestanding float helpers: the Guest links no libm.
[[nodiscard]] float Floor(float value) { return __builtin_floorf(value); }
[[nodiscard]] float Fabs(float value) { return __builtin_fabsf(value); }

// Odd polynomial on [-pi/2, pi/2] after range reduction; error below 1e-6.
[[nodiscard]] float Sin(float radians) {
    float x = radians - kTwoPi * Floor((radians + kPi) / kTwoPi);
    if (x > kHalfPi) {
        x = kPi - x;
    } else if (x < -kHalfPi) {
        x = -kPi - x;
    }
    const float x2 = x * x;
    return x * (1.0F + x2 * (-1.0F / 6.0F +
                             x2 * (1.0F / 120.0F +
                                   x2 * (-1.0F / 5040.0F + x2 * (1.0F / 362880.0F + x2 * (-1.0F / 39916800.0F))))));
}

[[nodiscard]] float Cos(float radians) { return Sin(radians + kHalfPi); }

[[nodiscard]] float Clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

[[nodiscard]] Rect Intersect(Rect a, Rect b) {
    const int32_t x0 = a.x > b.x ? a.x : b.x;
    const int32_t y0 = a.y > b.y ? a.y : b.y;
    const int32_t x1 = (a.x + a.width) < (b.x + b.width) ? (a.x + a.width) : (b.x + b.width);
    const int32_t y1 = (a.y + a.height) < (b.y + b.height) ? (a.y + a.height) : (b.y + b.height);
    return {x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
}

}  // namespace

float Vec3::Length() const {
    const float squared = x * x + y * y + z * z;
    return squared > 0.0F ? __builtin_sqrtf(squared) : 0.0F;
}

Transform3 Transform3::RotationX(float radians) {
    const float c = Cos(radians);
    const float s = Sin(radians);
    Transform3 result{};
    result.m[1][1] = c;
    result.m[1][2] = s;
    result.m[2][1] = -s;
    result.m[2][2] = c;
    return result;
}

Transform3 Transform3::RotationY(float radians) {
    const float c = Cos(radians);
    const float s = Sin(radians);
    Transform3 result{};
    result.m[0][0] = c;
    result.m[0][2] = s;
    result.m[2][0] = -s;
    result.m[2][2] = c;
    return result;
}

Transform3 Transform3::RotationZ(float radians) {
    const float c = Cos(radians);
    const float s = Sin(radians);
    Transform3 result{};
    result.m[0][0] = c;
    result.m[0][1] = -s;
    result.m[1][0] = s;
    result.m[1][1] = c;
    return result;
}

Transform3 Transform3::Uniform(Vec3 translation, float yaw, float pitch, float roll) {
    Transform3 result = RotationY(yaw) * RotationX(pitch) * RotationZ(roll);
    result.t = translation;
    return result;
}

float MeshCamera::FocalLength(float horizontal_fov_radians, int view_width) {
    const float half = Clamp(horizontal_fov_radians, 0.1F, 3.0F) * 0.5F;
    const float c = Cos(half);
    if (c <= 1e-4F) return static_cast<float>(view_width);
    return static_cast<float>(view_width) * 0.5F * c / Sin(half);
}

// ---- MeshRenderer -----------------------------------------------------------

bool MeshRenderer::Initialize(const MeshRendererConfig& config, Storage storage, uint8_t groups) {
    if (config.width <= 0 || config.height <= 0 || config.width > static_cast<int>(RasterVertex::kMaxPosition) ||
        config.height > static_cast<int>(RasterVertex::kMaxPosition) || config.far <= 0.0F ||
        config.lighting.levels == 0U || config.lighting.minimum >= config.lighting.levels ||
        config.lighting.dark_distance <= config.lighting.full_distance || config.subdivide_depth_ratio <= 1.0F ||
        groups == 0U || groups > kMaxGroups || storage.polygons.empty() || storage.polygons.size() >= kNoSlot ||
        storage.buckets.size() < static_cast<size_t>(groups) * kBuckets) {
        return false;
    }
    config_ = config;
    storage_ = storage;
    groups_ = groups;
    center_x_ = static_cast<float>(config.width) * 0.5F;
    center_y_ = static_cast<float>(config.height) * 0.5F;
    bucket_scale_ = static_cast<float>(kBuckets - 1U) / config.far;
    Begin(MeshCamera{});
    return true;
}

void MeshRenderer::Begin(const MeshCamera& camera) {
    camera_ = camera;
    if (camera_.near <= 0.0F) camera_.near = 0.01F;
    if (camera_.focal_length <= 0.0F) camera_.focal_length = 1.0F;
    // Camera local -> world is Ry(yaw) * Rx(pitch); world -> view is its
    // transpose applied to (world - position).
    const Transform3 orientation = Transform3::RotationY(camera.yaw) * Transform3::RotationX(camera.pitch);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            world_to_view_.m[row][column] = orientation.m[column][row];
        }
    }
    world_to_view_.t = world_to_view_.Rotate(camera.position * -1.0F);
    for (uint32_t index = 0U; index < static_cast<uint32_t>(groups_) * kBuckets; ++index) {
        storage_.buckets[index] = kNoSlot;
    }
    used_ = 0U;
    stats_ = Stats{};
}

Vec3 MeshRenderer::ToView(Vec3 world) const { return world_to_view_.Apply(world); }

bool MeshRenderer::Project(Vec3 world, float& x_out, float& y_out, float& depth_out) const {
    const Vec3 view = ToView(world);
    if (view.z < camera_.near) return false;
    const float scale = camera_.focal_length / view.z;
    x_out = center_x_ + view.x * scale;
    y_out = center_y_ - view.y * scale;
    depth_out = view.z;
    return true;
}

float MeshRenderer::LightFor(float brightness, float depth) const {
    const MeshLighting& lighting = config_.lighting;
    float attenuation = 1.0F;
    if (depth >= lighting.dark_distance) {
        attenuation = 0.0F;
    } else if (depth > lighting.full_distance) {
        attenuation = (lighting.dark_distance - depth) / (lighting.dark_distance - lighting.full_distance);
    }
    const float top = static_cast<float>(lighting.levels - 1U);
    return Clamp(brightness * (1.0F / 255.0F) * top * attenuation, static_cast<float>(lighting.minimum), top);
}

bool MeshRenderer::Submit(const Mesh& mesh, const Transform3& transform, const MeshSubmitOptions& options) {
    if (storage_.polygons.empty() || options.group >= groups_ || mesh.vertices.size() > kMaxMeshVertices) {
        return false;
    }
    const Transform3 local_to_view = world_to_view_ * transform;
    for (size_t index = 0U; index < mesh.vertices.size(); ++index) {
        const MeshVertex& v = mesh.vertices[index];
        view_vertices_[index] = local_to_view.Apply({v.x, v.y, v.z});
    }
    const Rect view{0, 0, config_.width, config_.height};
    Rect scissor = view;
    if (options.scissor.width > 0 && options.scissor.height > 0) {
        scissor = Intersect(options.scissor, view);
        if (scissor.width <= 0 || scissor.height <= 0) {
            stats_.faces += static_cast<uint32_t>(mesh.faces.size());
            stats_.culled += static_cast<uint32_t>(mesh.faces.size());
            return true;
        }
    }
    for (const MeshFace& face : mesh.faces) {
        ++stats_.faces;
        const uint32_t corners = face.corners();
        bool valid = true;
        for (uint32_t c = 0U; c < corners; ++c) valid = valid && face.vertex[c] < mesh.vertices.size();
        if (!valid) {
            ++stats_.culled;
            continue;
        }
        SubmitFace(face, options, scissor);
    }
    return true;
}

void MeshRenderer::SubmitFace(const MeshFace& face, const MeshSubmitOptions& options, Rect scissor) {
    const uint32_t corners = face.corners();
    ClipVertex input[4];
    uint32_t behind = 0U;
    float depth_sum = 0.0F;
    for (uint32_t c = 0U; c < corners; ++c) {
        ClipVertex& cv = input[c];
        cv.p = view_vertices_[face.vertex[c]];
        cv.u = static_cast<float>(face.u[c]);
        cv.v = static_cast<float>(face.v[c]);
        cv.light = LightFor(static_cast<float>(face.brightness[c]), cv.p.z);
        depth_sum += cv.p.z;
        if (cv.p.z < camera_.near) ++behind;
    }
    if (behind == corners) {
        ++stats_.culled;
        return;
    }
    const float depth = depth_sum / static_cast<float>(corners) + options.depth_bias;
    if (behind == 0U) {
        EmitViewPolygon(input, corners, face, options, scissor, depth, 0U);
        return;
    }
    // Sutherland-Hodgman against z = near; a convex n-gon gains at most one corner.
    ClipVertex output[kMaxClipVertices];
    uint32_t count = 0U;
    const float near = camera_.near;
    for (uint32_t c = 0U; c < corners; ++c) {
        const ClipVertex& a = input[c];
        const ClipVertex& b = input[c + 1U == corners ? 0U : c + 1U];
        const bool a_in = a.p.z >= near;
        const bool b_in = b.p.z >= near;
        if (a_in) output[count++] = a;
        if (a_in != b_in) {
            const float t = (near - a.p.z) / (b.p.z - a.p.z);
            ClipVertex& m = output[count++];
            m.p = a.p + (b.p - a.p) * t;
            m.p.z = near;
            m.u = a.u + (b.u - a.u) * t;
            m.v = a.v + (b.v - a.v) * t;
            m.light = a.light + (b.light - a.light) * t;
        }
    }
    ++stats_.clipped;
    if (count < 3U) {
        ++stats_.culled;
        return;
    }
    // A clipped polygon is never subdivided: pass the maximum level.
    EmitViewPolygon(output, count, face, options, scissor, depth, config_.subdivide_levels);
}

void MeshRenderer::EmitViewPolygon(const ClipVertex* vertices, uint32_t count, const MeshFace& face,
                                   const MeshSubmitOptions& options, Rect scissor, float depth, uint8_t level) {
    ScreenVertex screen[kMaxClipVertices];
    float min_x = 1e9F, max_x = -1e9F, min_y = 1e9F, max_y = -1e9F, min_z = 1e9F, max_z = -1e9F;
    for (uint32_t c = 0U; c < count; ++c) {
        const ClipVertex& cv = vertices[c];
        const float scale = camera_.focal_length / cv.p.z;
        ScreenVertex& sv = screen[c];
        sv.x = center_x_ + cv.p.x * scale;
        sv.y = center_y_ - cv.p.y * scale;
        sv.u = cv.u;
        sv.v = cv.v;
        sv.light = cv.light;
        min_x = sv.x < min_x ? sv.x : min_x;
        max_x = sv.x > max_x ? sv.x : max_x;
        min_y = sv.y < min_y ? sv.y : min_y;
        max_y = sv.y > max_y ? sv.y : max_y;
        min_z = cv.p.z < min_z ? cv.p.z : min_z;
        max_z = cv.p.z > max_z ? cv.p.z : max_z;
    }
    // Screen-space winding (y down): a face listed counter-clockwise from its
    // front projects with negative doubled area when the front faces the camera.
    float area2 = 0.0F;
    for (uint32_t c = 0U; c < count; ++c) {
        const ScreenVertex& a = screen[c];
        const ScreenVertex& b = screen[c + 1U == count ? 0U : c + 1U];
        area2 += a.x * b.y - b.x * a.y;
    }
    const bool double_sided = (face.flags & kMeshFaceDoubleSided) != 0U;
    if (area2 == 0.0F || (!double_sided && area2 > 0.0F)) {
        ++stats_.culled;
        return;
    }
    if (max_x <= static_cast<float>(scissor.x) || min_x >= static_cast<float>(scissor.x + scissor.width) ||
        max_y <= static_cast<float>(scissor.y) || min_y >= static_cast<float>(scissor.y + scissor.height)) {
        ++stats_.culled;
        return;
    }
    if (level < config_.subdivide_levels && count <= 4U && max_z > min_z * config_.subdivide_depth_ratio &&
        (max_x - min_x > static_cast<float>(config_.subdivide_min_pixels) ||
         max_y - min_y > static_cast<float>(config_.subdivide_min_pixels))) {
        ++stats_.subdivided;
        const auto midpoint = [](const ClipVertex& a, const ClipVertex& b) {
            return ClipVertex{(a.p + b.p) * 0.5F, (a.u + b.u) * 0.5F, (a.v + b.v) * 0.5F, (a.light + b.light) * 0.5F};
        };
        const uint8_t next = static_cast<uint8_t>(level + 1U);
        if (count == 4U) {
            const ClipVertex m01 = midpoint(vertices[0], vertices[1]);
            const ClipVertex m12 = midpoint(vertices[1], vertices[2]);
            const ClipVertex m23 = midpoint(vertices[2], vertices[3]);
            const ClipVertex m30 = midpoint(vertices[3], vertices[0]);
            const ClipVertex centre = midpoint(m01, m23);
            const ClipVertex quads[4][4] = {{vertices[0], m01, centre, m30},
                                            {m01, vertices[1], m12, centre},
                                            {centre, m12, vertices[2], m23},
                                            {m30, centre, m23, vertices[3]}};
            for (const auto& quad : quads) EmitViewPolygon(quad, 4U, face, options, scissor, depth, next);
        } else {
            const ClipVertex m01 = midpoint(vertices[0], vertices[1]);
            const ClipVertex m12 = midpoint(vertices[1], vertices[2]);
            const ClipVertex m20 = midpoint(vertices[2], vertices[0]);
            const ClipVertex triangles[4][3] = {
                {vertices[0], m01, m20}, {m01, vertices[1], m12}, {m20, m12, vertices[2]}, {m01, m12, m20}};
            for (const auto& triangle : triangles) {
                EmitViewPolygon(triangle, 3U, face, options, scissor, depth, next);
            }
        }
        return;
    }
    EmitScreenPolygon(screen, count, face, options.group, depth, scissor);
}

void MeshRenderer::EmitScreenPolygon(ScreenVertex* vertices, uint32_t count, const MeshFace& face, uint8_t group,
                                     float depth, Rect scissor) {
    // Exact clip against the scissor edges in screen space. Attributes are
    // affine in screen space by construction, so linear interpolation here is
    // what the Host would have sampled.
    const float edges[4] = {static_cast<float>(scissor.x), static_cast<float>(scissor.x + scissor.width),
                            static_cast<float>(scissor.y), static_cast<float>(scissor.y + scissor.height)};
    bool inside = true;
    for (uint32_t c = 0U; c < count && inside; ++c) {
        inside = vertices[c].x >= edges[0] && vertices[c].x <= edges[1] && vertices[c].y >= edges[2] &&
                 vertices[c].y <= edges[3];
    }
    ScreenVertex scratch[kMaxClipVertices];
    if (!inside) {
        ++stats_.clipped;
        ScreenVertex* in = vertices;
        ScreenVertex* out = scratch;
        for (uint32_t edge = 0U; edge < 4U && count >= 3U; ++edge) {
            const bool horizontal = edge >= 2U;  // clipping against a y bound
            const bool keep_greater = (edge & 1U) == 0U;
            const float bound = edges[edge];
            uint32_t produced = 0U;
            for (uint32_t c = 0U; c < count && produced + 2U <= kMaxClipVertices; ++c) {
                const ScreenVertex& a = in[c];
                const ScreenVertex& b = in[c + 1U == count ? 0U : c + 1U];
                const float da = (horizontal ? a.y : a.x) - bound;
                const float db = (horizontal ? b.y : b.x) - bound;
                const bool a_in = keep_greater ? da >= 0.0F : da <= 0.0F;
                const bool b_in = keep_greater ? db >= 0.0F : db <= 0.0F;
                if (a_in) out[produced++] = a;
                if (a_in != b_in) {
                    const float t = da / (da - db);
                    ScreenVertex& m = out[produced++];
                    m.x = a.x + (b.x - a.x) * t;
                    m.y = a.y + (b.y - a.y) * t;
                    if (horizontal) {
                        m.y = bound;
                    } else {
                        m.x = bound;
                    }
                    m.u = a.u + (b.u - a.u) * t;
                    m.v = a.v + (b.v - a.v) * t;
                    m.light = a.light + (b.light - a.light) * t;
                }
            }
            count = produced;
            ScreenVertex* swap = in;
            in = out;
            out = swap;
        }
        vertices = in;
        if (count < 3U) {
            ++stats_.culled;
            return;
        }
    }
    // Fan from vertex 0: quads while three more corners remain, a triangle for
    // the last two. Every piece of a convex polygon is convex.
    uint32_t next = 1U;
    while (count - next >= 2U) {
        if (count - next >= 3U) {
            const ScreenVertex quad[4] = {vertices[0], vertices[next], vertices[next + 1U], vertices[next + 2U]};
            Queue(quad, 4U, face, group, depth);
            next += 2U;
        } else {
            const ScreenVertex triangle[3] = {vertices[0], vertices[next], vertices[next + 1U]};
            Queue(triangle, 3U, face, group, depth);
            next += 1U;
        }
    }
}

void MeshRenderer::Queue(const ScreenVertex* vertices, uint32_t count, const MeshFace& face, uint8_t group,
                         float depth) {
    if (used_ >= storage_.polygons.size()) {
        ++stats_.dropped;
        return;
    }
    // The wire holds 8.8 texels: shift by a multiple of 256 so the smallest
    // corner lands in [0, 256). Every power-of-two texture repeats on 256.
    float min_u = vertices[0].u, min_v = vertices[0].v;
    float area2 = 0.0F;
    for (uint32_t c = 0U; c < count; ++c) {
        min_u = vertices[c].u < min_u ? vertices[c].u : min_u;
        min_v = vertices[c].v < min_v ? vertices[c].v : min_v;
        const ScreenVertex& a = vertices[c];
        const ScreenVertex& b = vertices[c + 1U == count ? 0U : c + 1U];
        area2 += a.x * b.y - b.x * a.y;
    }
    const float shift_u = Floor(min_u * (1.0F / 256.0F)) * 256.0F;
    const float shift_v = Floor(min_v * (1.0F / 256.0F)) * 256.0F;
    const float top = static_cast<float>(config_.lighting.levels - 1U);
    MeshPolygonSlot& slot = storage_.polygons[used_];
    for (uint32_t c = 0U; c < count; ++c) {
        const ScreenVertex& sv = vertices[c];
        const auto light = static_cast<uint8_t>(Clamp(sv.light + 0.5F, 0.0F, top));
        slot.corners[c] = RasterVertex::At(sv.x, sv.y, sv.u - shift_u, sv.v - shift_v, light);
    }
    slot.count = static_cast<uint8_t>(count);
    slot.flags = face.flags;
    // Flat colour polygons carry their palette index where a texture slot
    // would otherwise go.
    slot.texture_slot = (face.flags & kMeshFaceFlatColor) != 0U ? static_cast<uint8_t>(face.u[0]) : face.texture_slot;
    const float key = Clamp(depth, 0.0F, config_.far) * bucket_scale_;
    const uint32_t bucket = static_cast<uint32_t>(key);
    uint16_t& head = storage_.buckets[static_cast<uint32_t>(group) * kBuckets + bucket];
    slot.next = head;
    head = static_cast<uint16_t>(used_);
    ++used_;
    ++stats_.polygons;
    stats_.pixel_estimate += static_cast<uint32_t>(Fabs(area2) * 0.5F);
}

bool MeshRenderer::Flush(RasterDrawList& list) {
    bool ok = true;
    for (uint32_t group = 0U; group < groups_; ++group) {
        const uint16_t* heads = storage_.buckets.data() + group * kBuckets;
        for (uint32_t bucket = kBuckets; bucket-- > 0U;) {
            for (uint16_t index = heads[bucket]; index != kNoSlot; index = storage_.polygons[index].next) {
                const MeshPolygonSlot& slot = storage_.polygons[index];
                const bool transparent = (slot.flags & kMeshFaceTransparent) != 0U;
                const bool flat = (slot.flags & kMeshFaceFlatColor) != 0U;
                if (slot.count == 4U) {
                    ok = (flat ? list.FlatQuad(slot.corners, slot.texture_slot)
                               : list.Quad(slot.corners, slot.texture_slot, transparent)) &&
                         ok;
                } else {
                    const RasterVertex triangle[3] = {slot.corners[0], slot.corners[1], slot.corners[2]};
                    ok = (flat ? list.FlatTriangle(triangle, slot.texture_slot)
                               : list.Triangle(triangle, slot.texture_slot, transparent)) &&
                         ok;
                }
            }
        }
    }
    return ok;
}

}  // namespace micropixel
