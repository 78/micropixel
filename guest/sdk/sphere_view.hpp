#ifndef MICROPIXEL_SDK_SPHERE_VIEW_HPP
#define MICROPIXEL_SDK_SPHERE_VIEW_HPP

#include <stdint.h>

namespace micropixel {

// A textured sphere seen along the view axis, for globes, planets and balls
// wrapped in an equirectangular texture. The App uploads the texture and a lit
// palette through RasterResources once, has SphereView fill a warp map (one
// WarpEntry per pixel of the diameter x diameter disc), uploads it, and then
// draws the whole sphere each frame with one RasterDrawList::Warp record whose
// u_offset is YawOffset(): spinning costs nothing. Tilting (pitch) changes the
// map; BuildRows() is written so the rebuild can be spread over frames and
// streamed with RasterResources::UpdateWarpRows().
//
// View space: x right, y down, z toward the viewer. The sphere's axis starts
// along -y and is tilted about x by `tilt` + pitch(); yaw spins it about that
// axis. Texture u runs with longitude, v from the north pole (0) to the south
// pole (texture_height - 1).
struct SphereViewConfig final {
    // Disc diameter in target pixels, at most SphereView::kMaxDiameter; the
    // warp map is diameter x diameter.
    int32_t diameter{};
    // Equirectangular texture size; both powers of two (WARP samples with masks).
    uint16_t texture_width{};
    uint16_t texture_height{};
    // Rows above and below the map: latitude +-90 degrees land on rows
    // polar_padding and texture_height - 1 - polar_padding. Padded rows hold
    // the pole colour so a v_offset shift shows more pole instead of wrapping
    // to the other hemisphere. 0 uses the whole texture height.
    uint16_t polar_padding{};
    // Texel fraction bits of u in the map and in YawOffset(): the Warp
    // record's u_fraction_bits. 2 lets the globe spin in quarter-texel steps
    // (smooth at low speeds) and samples the texture more exactly;
    // texture_width << u_fraction_bits must stay within 4096.
    uint8_t u_fraction_bits{};
    // Light levels in the palette the Warp record will use; entries use
    // 0..light_levels - 1 from the dark limb to the lit centre.
    uint8_t light_levels{1};
    // Axial tilt in radians, applied before the interactive pitch.
    float tilt{};
    // Direction toward the light in view space; need not be normalized.
    float light_x{-0.42F};
    float light_y{0.50F};
    float light_z{0.76F};
    // Shade = ambient + (1 - ambient) * max(0, n . light), then darkened toward
    // the limb by limb_darkening * d^4 where d is the normalized disc radius.
    float ambient{0.36F};
    float limb_darkening{0.16F};
};

// Rotation for one yaw, so many surface points can be projected per frame
// with a handful of multiplies each. Obtain from SphereView::Projector().
class SphereProjector final {
   public:
    // Projects a unit vector in the sphere's own frame (SphereView::UnitVector).
    // x/y are pixels relative to the warp map's (0, 0) entry, facing is the
    // view-space z of the surface normal (1 at the disc centre, 0 on the
    // limb). False when the point is on the far side; outputs unspecified.
    [[nodiscard]] bool Project(float ux, float uy, float uz, float& x, float& y, float& facing) const;

   private:
    friend class SphereView;
    float cos_yaw_{};
    float sin_yaw_{};
    float cos_tilt_{};
    float sin_tilt_{};
    float radius_{};
};

class SphereView final {
   public:
    // False (and the view stays unusable) when the diameter is not in
    // 1..kMaxDiameter, u_fraction_bits is above 4 or does not leave the
    // texture width within 4096, a texture dimension is not a power of two in 1..4096, or light_levels
    // is 0 or above WarpEntry::kMaxLight + 1.
    [[nodiscard]] bool Initialize(const SphereViewConfig& config);
    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] const SphereViewConfig& config() const { return config_; }

    // Interactive tilt about the view x axis, added to the config tilt. Takes
    // effect in BuildRows() and Project(); the uploaded map does not change
    // until rebuilt rows are uploaded.
    void SetPitch(float pitch) { pitch_ = pitch; }
    [[nodiscard]] float pitch() const { return pitch_; }

    // How finely BuildRows() samples the texture coordinates. kCoarse computes
    // them for one pixel in each 2x2 block and repeats it for the other three
    // (about 4x faster), for rebuilding while the pitch is being dragged; the
    // disc edge and the shading stay per pixel. kFull is exact per pixel.
    enum class Detail : uint8_t { kFull, kCoarse };

    // The pitch-independent part of the map: the light level of every pixel,
    // kOutside off the disc. Writes row_count * diameter bytes for map rows
    // first_row.. into `out`. Build it once and hand it to BuildRows() so
    // pitch rebuilds skip the shading and edge math.
    static constexpr uint8_t kOutside = 0xFF;
    // Largest supported diameter (map width) in pixels.
    static constexpr int32_t kMaxDiameter = 1024;
    void BuildShades(int32_t first_row, int32_t row_count, uint8_t* out) const;

    // Writes row_count * diameter entries for map rows first_row.. into
    // `out`: WarpEntry::Texel for pixels on the disc, WarpEntry::kSkip
    // outside it. Rows are clamped to the map. The right half of each row
    // mirrors the left half's longitude, so half the pixels need trigonometry.
    // `shades`, when given, holds these rows' BuildShades() output; any byte
    // with bit 7 set is written as kSkip, so callers may mark pixels they
    // paint themselves. `stride` is the distance between rows of `out` (in
    // entries) and of `shades` (in bytes); 0 means diameter. Pass as many
    // rows at once as possible: kCoarse copies odd rows from the row above
    // only within one call.
    void BuildRows(int32_t first_row, int32_t row_count, uint32_t* out, Detail detail = Detail::kFull,
                   const uint8_t* shades = nullptr, int32_t stride = 0) const;

    // Warp record u_offset that spins the texture by `yaw` radians, in units
    // of 1 / 2^u_fraction_bits texel; pass u_fraction_bits() with it.
    [[nodiscard]] uint16_t YawOffset(float yaw) const;
    [[nodiscard]] uint8_t u_fraction_bits() const { return config_.u_fraction_bits; }

    // Projection for the current pitch and `yaw`; valid until either changes.
    [[nodiscard]] SphereProjector Projector(float yaw) const;
    // Projects one surface point at latitude/longitude (radians; north and
    // east positive); see SphereProjector::Project for the outputs.
    [[nodiscard]] bool Project(float latitude, float longitude, float yaw, float& x, float& y, float& facing) const;

    // Unit vector for a latitude/longitude in the sphere's own frame, which
    // Apps cache per object and feed to SphereProjector::Project.
    static void UnitVector(float latitude, float longitude, float& x, float& y, float& z);

   private:
    SphereViewConfig config_{};
    float pitch_{};
    float radius_{};  // disc radius in pixels
    float centre_{};  // disc centre coordinate (same for x and y)
    float light_x_{};
    float light_y_{};
    float light_z_{};
    bool valid_{};
};

}  // namespace micropixel

#endif
