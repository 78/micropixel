#include "sdk/sphere_view.hpp"

#include "sdk/graphics.hpp"

namespace micropixel {
namespace {

constexpr float kPi = 3.14159265F;

// The Guest links no libm; these approximations (max error ~1e-3 rad) are
// plenty for a table of texel indices and a projection to whole pixels.
[[nodiscard]] float Sqrt(float value) { return value > 0.0F ? __builtin_sqrtf(value) : 0.0F; }
[[nodiscard]] float Fabs(float value) { return __builtin_fabsf(value); }
[[nodiscard]] float Clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

[[nodiscard]] float Sin(float x) {
    float t = x * (1.0F / (2.0F * kPi));
    t -= static_cast<float>(static_cast<int32_t>(t));
    if (t < 0.0F) t += 1.0F;
    const float h = t < 0.5F ? t : t - 0.5F;
    const float u = h * 2.0F;
    float p = 4.0F * u * (1.0F - u);
    p = p * (0.775F + 0.225F * p);
    return t < 0.5F ? p : -p;
}

[[nodiscard]] float Cos(float x) { return Sin(x + kPi * 0.5F); }

[[nodiscard]] float Asin(float x) {
    const float a = Fabs(x);
    const float r =
        kPi * 0.5F - Sqrt(1.0F - a) * (1.5707288F - 0.2121144F * a + 0.0742610F * a * a - 0.0187293F * a * a * a);
    return x < 0.0F ? -r : r;
}

[[nodiscard]] float Atan2(float y, float x) {
    const float ax = Fabs(x);
    const float ay = Fabs(y);
    const float d = ax > ay ? ax : ay;
    if (d < 1e-6F) return 0.0F;
    const float a = (ax < ay ? ax : ay) / d;
    const float s = a * a;
    float r = ((-0.0464964749F * s + 0.15931422F) * s - 0.327622764F) * s * a + a;
    if (ay > ax) r = kPi * 0.5F - r;
    if (x < 0.0F) r = kPi - r;
    return y < 0.0F ? -r : r;
}

[[nodiscard]] bool PowerOfTwoTextureSize(uint32_t size) {
    return size != 0U && size <= WarpEntry::kMaxCoordinate + 1U && (size & (size - 1U)) == 0U;
}

}  // namespace

bool SphereView::Initialize(const SphereViewConfig& config) {
    valid_ = false;
    if (config.diameter <= 0 || config.diameter > kMaxDiameter || !PowerOfTwoTextureSize(config.texture_width) ||
        !PowerOfTwoTextureSize(config.texture_height) || config.u_fraction_bits > 4U ||
        (static_cast<uint32_t>(config.texture_width) << config.u_fraction_bits) > WarpEntry::kMaxCoordinate + 1U ||
        config.light_levels == 0U || config.light_levels > WarpEntry::kMaxLight + 1U ||
        2U * static_cast<uint32_t>(config.polar_padding) + 2U > config.texture_height) {
        return false;
    }
    const float length =
        Sqrt(config.light_x * config.light_x + config.light_y * config.light_y + config.light_z * config.light_z);
    if (!(length > 0.0F)) return false;
    config_ = config;
    light_x_ = config.light_x / length;
    light_y_ = config.light_y / length;
    light_z_ = config.light_z / length;
    // One pixel of margin keeps the limb inside the map after rounding.
    radius_ = static_cast<float>(config.diameter) * 0.5F - 1.0F;
    centre_ = static_cast<float>(config.diameter) * 0.5F - 0.5F;
    pitch_ = 0.0F;
    valid_ = true;
    return true;
}

void SphereView::BuildShades(int32_t first_row, int32_t row_count, uint8_t* out) const {
    if (!valid_ || out == nullptr) return;
    const int32_t diameter = config_.diameter;
    if (first_row < 0) first_row = 0;
    const int32_t last = first_row + row_count < diameter ? first_row + row_count : diameter;
    const float inv_radius = 1.0F / radius_;
    const float shade_scale = static_cast<float>(config_.light_levels - 1U);
    const float diffuse = 1.0F - config_.ambient;
    for (int32_t row = first_row; row < last; ++row) {
        const float ny = (static_cast<float>(row) - centre_) * inv_radius;
        for (int32_t column = 0; column < diameter; ++column, ++out) {
            const float nx = (static_cast<float>(column) - centre_) * inv_radius;
            const float d2 = nx * nx + ny * ny;
            if (d2 > 1.0F) {
                *out = kOutside;
                continue;
            }
            const float nz = Sqrt(1.0F - d2);
            const float lambert = Clamp(nx * light_x_ + (-ny) * light_y_ + nz * light_z_, 0.0F, 1.0F);
            const float lit = config_.ambient + diffuse * lambert;
            const float limb = 1.0F - config_.limb_darkening * d2 * d2;
            const int32_t shade = static_cast<int32_t>(lit * limb * shade_scale + 0.5F);
            *out = static_cast<uint8_t>(shade < 0 ? 0 : shade);
        }
    }
}

void SphereView::BuildRows(int32_t first_row, int32_t row_count, uint32_t* out, Detail detail, const uint8_t* shades,
                           int32_t stride) const {
    if (!valid_ || out == nullptr) return;
    const int32_t diameter = config_.diameter;
    if (stride < diameter) stride = diameter;
    if (first_row < 0) first_row = 0;
    const int32_t last = first_row + row_count < diameter ? first_row + row_count : diameter;
    const bool coarse = detail == Detail::kCoarse;
    constexpr uint32_t kVMask = WarpEntry::kMaxCoordinate << 12U;
    const float cos_tilt = Cos(config_.tilt + pitch_);
    const float sin_tilt = Sin(config_.tilt + pitch_);
    // u is in 1 / 2^u_fraction_bits texel units: the scaled width wraps it.
    const uint32_t width = static_cast<uint32_t>(config_.texture_width) << config_.u_fraction_bits;
    const float texture_width = static_cast<float>(width);
    const int32_t first_row_v = config_.polar_padding;
    const int32_t last_row_v = static_cast<int32_t>(config_.texture_height) - 1 - config_.polar_padding;
    const float band_v = static_cast<float>(last_row_v - first_row_v);
    const float inv_radius = 1.0F / radius_;
    const uint32_t u_mask = width - 1U;
    // Columns 0..half are computed; the rest mirror them (the centre column
    // of an odd diameter is its own mirror).
    const int32_t half = (diameter - 1) / 2;
    // Shades for the current row when the caller supplied none.
    uint8_t local_shades[kMaxDiameter];

    // Texture coordinates of one pixel: rotate the view-space normal into the
    // sphere frame (undoing the tilt) and read latitude and longitude.
    auto texel_of = [&](int32_t column, float ny) -> uint32_t {
        const float nx = (static_cast<float>(column) - centre_) * inv_radius;
        const float d2 = nx * nx + ny * ny;
        const float nz = Sqrt(d2 < 1.0F ? 1.0F - d2 : 0.0F);
        const float wy = ny * cos_tilt - nz * sin_tilt;
        const float wz = ny * sin_tilt + nz * cos_tilt;
        const float latitude = Asin(Clamp(-wy, -1.0F, 1.0F));
        const float longitude = Atan2(nx, wz);
        const int32_t v = first_row_v + static_cast<int32_t>((0.5F - latitude / kPi) * band_v + 0.5F);
        const int32_t u = static_cast<int32_t>((longitude / (2.0F * kPi) + 0.5F) * texture_width + 0.5F);
        const int32_t v_clamped = v < first_row_v ? first_row_v : (v > last_row_v ? last_row_v : v);
        return WarpEntry::Texel(static_cast<uint32_t>(u) & u_mask, static_cast<uint32_t>(v_clamped), 0U);
    };
    // Entry for one pixel inside the disc range: skip when the shade byte
    // marks it, otherwise its texel with the shade as light.
    auto entry_of = [&](int32_t column, float ny, uint8_t shade) -> uint32_t {
        if ((shade & 0x80U) != 0U) return WarpEntry::kSkip;
        return texel_of(column, ny) | (static_cast<uint32_t>(shade) << 24U);
    };

    for (int32_t row = first_row; row < last; ++row, out += stride) {
        const float ny = (static_cast<float>(row) - centre_) * inv_radius;
        const uint8_t* shade_row = shades;
        if (shades != nullptr) {
            shades += stride;
        } else {
            BuildShades(row, 1, local_shades);
            shade_row = local_shades;
        }
        // The disc occupies the columns [x0, x1) of this row; everything
        // outside is skipped without looking at it again.
        int32_t x0 = 0;
        while (x0 < diameter && (shade_row[x0] & 0x80U) != 0U) ++x0;
        int32_t x1 = diameter;
        while (x1 > x0 && (shade_row[x1 - 1] & 0x80U) != 0U) --x1;
        for (int32_t column = 0; column < x0; ++column) out[column] = WarpEntry::kSkip;
        for (int32_t column = x1; column < diameter; ++column) out[column] = WarpEntry::kSkip;
        if (x1 <= x0) continue;

        // Coarse odd rows copy the row above (light included) when it is in
        // this call; only the edge pixels the row above skips are computed.
        if (coarse && (row & 1) != 0 && row > first_row) {
            const uint32_t* above = out - stride;
            __builtin_memcpy(out + x0, above + x0, static_cast<size_t>(x1 - x0) * sizeof(uint32_t));
            for (int32_t column = x0; column < x1 && static_cast<int32_t>(out[column]) < 0; ++column) {
                out[column] = entry_of(column, ny, shade_row[column]);
            }
            for (int32_t column = x1 - 1; column > x0 && static_cast<int32_t>(out[column]) < 0; --column) {
                out[column] = entry_of(column, ny, shade_row[column]);
            }
            continue;
        }
        // Left half: trigonometry, or in coarse mode every other column with
        // the odd column repeating its left neighbour.
        const int32_t left_end = half < x1 - 1 ? half : x1 - 1;
        for (int32_t column = x0; column <= left_end; ++column) {
            const uint8_t shade = shade_row[column];
            if (coarse && (column & 1) != 0 && (shade & 0x80U) == 0U && static_cast<int32_t>(out[column - 1]) >= 0) {
                out[column] = (out[column - 1] & ~(0xFFU << 24U)) | (static_cast<uint32_t>(shade) << 24U);
            } else {
                out[column] = entry_of(column, ny, shade);
            }
        }
        // Right half mirrors the left: same latitude, negated longitude.
        const int32_t right_begin = half + 1 > x0 ? half + 1 : x0;
        for (int32_t column = right_begin; column < x1; ++column) {
            const uint8_t shade = shade_row[column];
            const uint32_t source = out[diameter - 1 - column];
            if ((shade & 0x80U) != 0U || static_cast<int32_t>(source) < 0) {
                out[column] = entry_of(column, ny, shade);
                continue;
            }
            out[column] =
                (source & kVMask) | ((width - (source & u_mask)) & u_mask) | (static_cast<uint32_t>(shade) << 24U);
        }
    }
}

uint16_t SphereView::YawOffset(float yaw) const {
    if (!valid_) return 0U;
    const uint32_t width = static_cast<uint32_t>(config_.texture_width) << config_.u_fraction_bits;
    const float columns = yaw * (static_cast<float>(width) / (2.0F * kPi));
    return static_cast<uint16_t>(static_cast<int32_t>(columns) & static_cast<int32_t>(width - 1U));
}

void SphereView::UnitVector(float latitude, float longitude, float& x, float& y, float& z) {
    const float cos_latitude = Cos(latitude);
    x = cos_latitude * Sin(longitude);
    y = -Sin(latitude);
    z = cos_latitude * Cos(longitude);
}

SphereProjector SphereView::Projector(float yaw) const {
    SphereProjector projector{};
    if (!valid_) return projector;
    projector.cos_yaw_ = Cos(-yaw);
    projector.sin_yaw_ = Sin(-yaw);
    projector.cos_tilt_ = Cos(config_.tilt + pitch_);
    projector.sin_tilt_ = Sin(config_.tilt + pitch_);
    projector.radius_ = static_cast<float>(config_.diameter) * 0.5F;
    return projector;
}

bool SphereView::Project(float latitude, float longitude, float yaw, float& x, float& y, float& facing) const {
    float ux = 0.0F;
    float uy = 0.0F;
    float uz = 0.0F;
    UnitVector(latitude, longitude, ux, uy, uz);
    return Projector(yaw).Project(ux, uy, uz, x, y, facing);
}

bool SphereProjector::Project(float ux, float uy, float uz, float& x, float& y, float& facing) const {
    if (!(radius_ > 0.0F)) return false;
    // Spin about the axis, then tilt about view x: the inverse of BuildRows.
    const float rx = ux * cos_yaw_ + uz * sin_yaw_;
    const float rz = -ux * sin_yaw_ + uz * cos_yaw_;
    const float vy = uy * cos_tilt_ + rz * sin_tilt_;
    const float vz = -uy * sin_tilt_ + rz * cos_tilt_;
    if (vz <= 0.0F) return false;
    x = radius_ + rx * radius_;
    y = radius_ + vy * radius_;
    facing = vz;
    return true;
}

}  // namespace micropixel
