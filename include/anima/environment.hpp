#pragma once
#include <anima/core/math.hpp>
#include <cstdint>

namespace anima {
// Linear RGB. Directions point from the surface toward the light source.
struct DirectionalLight {
    Vec3 direction{-.6F, .9F, .8F};
    Vec3 radiance{1.570796327F, 1.570796327F, 1.570796327F};
};
struct DirectionalShadow {
    bool enabled = false;
    Vec3 center{};
    float extent = 30, depth = 100;
    std::uint32_t resolution = 2048;
    // Receiver bias in normalized shadow depth; slope term increases at grazing angles.
    float constant_bias = .0005F, slope_bias = .0015F;
};
// Lighting-independent configuration shared by scene and immediate environments.
struct EnvironmentSettings {
    Vec3 ambient_sky{.4F, .4F, .4F}, ambient_ground{.4F, .4F, .4F};
    Vec3 ambient_specular{.08F, .08F, .08F};
    bool sky = false;
    Vec3 sky_zenith{.10F, .25F, .48F}, sky_horizon{.55F, .65F, .75F}, sky_ground{.12F, .15F, .18F};
    Vec3 fog_color{.55F, .65F, .75F};
    float fog_density = 0;
    float exposure = 1;
    bool tone_mapping = false;
    DirectionalShadow shadow;
    // Optional finer region, blended into the world shadow at its boundary.
    // Applications can follow their subject by updating this region's center.
    DirectionalShadow detail_shadow;
};
struct Environment : EnvironmentSettings {
    // Defaults reproduce the existing studio illumination.
    DirectionalLight sun;
    DirectionalLight fill{{.8F, .3F, -.5F}, {.314159265F, .314159265F, .314159265F}};
};
inline void validate_environment_settings(const EnvironmentSettings &e) {
    const auto color = [](Vec3 c) {
        return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && c.x >= 0 && c.y >= 0 && c.z >= 0;
    };
    for (const auto c :
         {e.ambient_sky, e.ambient_ground, e.ambient_specular, e.sky_zenith, e.sky_horizon, e.sky_ground, e.fog_color})
        if (!color(c))
            throw std::invalid_argument("Environment colours must be finite nonnegative linear RGB");
    if (!std::isfinite(e.fog_density) || e.fog_density < 0 || !std::isfinite(e.exposure) || e.exposure <= 0)
        throw std::invalid_argument("Invalid environment exposure or fog density");
    for (const auto &s : {e.shadow, e.detail_shadow})
        if (!std::isfinite(s.center.x) || !std::isfinite(s.center.y) || !std::isfinite(s.center.z) ||
            !std::isfinite(s.extent) || s.extent <= 0 || !std::isfinite(s.depth) || s.depth <= 0 || !s.resolution ||
            !std::isfinite(s.constant_bias) || s.constant_bias < 0 || !std::isfinite(s.slope_bias) || s.slope_bias < 0)
            throw std::invalid_argument("Invalid directional shadow region");
}
inline void validate_environment(const Environment &e) {
    validate_environment_settings(e);
    for (const auto &l : {e.sun, e.fill}) {
        const auto square = dot(l.direction, l.direction);
        const auto c = l.radiance;
        if (!std::isfinite(square) || square < 1e-12F || !std::isfinite(c.x) || !std::isfinite(c.y) ||
            !std::isfinite(c.z) || c.x < 0 || c.y < 0 || c.z < 0)
            throw std::invalid_argument("Invalid directional light");
    }
}
inline Mat4 directional_shadow_matrix(const Environment &e, bool detail = false) {
    validate_environment(e);
    const auto &region = detail ? e.detail_shadow : e.shadow;
    const auto forward = normalized(e.sun.direction) * -1.F;
    const auto up_hint = std::abs(forward.y) > .99F ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const auto right = normalized(cross(forward, up_hint)), up = cross(right, forward);
    auto center = region.center;
    // Snap the light-space origin to shadow texels to keep a following region stable.
    const auto texel = 2.F * region.extent / static_cast<float>(region.resolution);
    if (!std::isfinite(texel) || texel <= 0)
        throw std::invalid_argument("Shadow texel size exceeds finite range");
    center = center + right * (std::round(dot(right, center) / texel) * texel - dot(right, center)) +
             up * (std::round(dot(up, center) / texel) * texel - dot(up, center));
    const auto eye = center - forward * (region.depth * .5F);
    const Mat4 view{right.x, up.x, -forward.x, 0, right.y,          up.y,          -forward.y,        0,
                    right.z, up.z, -forward.z, 0, -dot(right, eye), -dot(up, eye), dot(forward, eye), 1};
    const float r = region.extent, d = region.depth;
    const Mat4 projection{1 / r, 0, 0, 0, 0, -1 / r, 0, 0, 0, 0, -1 / d, 0, 0, 0, 0, 1};
    const auto matrix = projection * view;
    for (const auto value : matrix)
        if (!std::isfinite(value))
            throw std::invalid_argument("Shadow matrix exceeds finite range");
    (void)inverse(matrix);
    return matrix;
}
} // namespace anima
