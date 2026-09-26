#pragma once
#include <anima/core/math.hpp>
#include <cstdint>
#include <numbers>

/// @file
/// Lighting environment values for anima::VulkanRenderer, with their validation. Header-only; needs only
/// `anima::core`.
///
/// Colors and radiance are linear RGB, and directions point from the surface toward the light. Validation
/// throws `std::invalid_argument`, or anima::MathError (derived from it) for a shadow projection that cannot
/// be inverted.

namespace anima {
/// One directional light of an Environment.
///
/// Lit surfaces reflect `BRDF * radiance * max(dot(N, L), 0)`, where `L` is the normalized #direction,
/// further scaled by shadow visibility for Environment::sun.
struct DirectionalLight {
    /// Direction from the surface toward the light, normalized when used. Its squared length must be finite
    /// and at least `1e-12`.
    Vec3 direction{-.6F, .9F, .8F};
    /// Finite, nonnegative radiance.
    Vec3 radiance{std::numbers::pi_v<float> / 2, std::numbers::pi_v<float> / 2, std::numbers::pi_v<float> / 2};
};
/// One orthographic shadow region of Environment::sun, fixed in world space.
///
/// The region is a box around #center, `2 * extent` wide on both axes across the light and #depth deep
/// along it, so a texel spans `2 * extent / resolution`. The center snaps to whole texels across the light,
/// which reduces shimmer when a region follows a subject. Only casters inside the box cast. Receivers outside
/// it fall back to no shadow for EnvironmentSettings::shadow and to the world region for
/// EnvironmentSettings::detail_shadow, and the region blends into that fallback over the outer 4 percent of
/// the box on each axis.
///
/// Filtering takes a bilinearly weighted 3x3 comparison kernel over 4x4 texels. Each texel is compared with
/// the receiver plane's depth at that texel, found from screen-space position derivatives rather than
/// shading normals, less the bias; nearly edge-on planes fall back to the bias alone.
///
/// A disabled region is validated like an enabled one: validate_environment_settings() checks every field and
/// directional_shadow_matrix() its projection. Only the device limit that VulkanRenderer::set_environment
/// places on #resolution is skipped while the region is disabled.
struct DirectionalShadow {
    /// Renders the region's casters into its depth map each frame. A disabled region casts nothing and keeps
    /// only a 1x1 placeholder map.
    bool enabled = false;
    /// World-space center, finite.
    Vec3 center{};
    /// Half the region's width across the light, finite and positive.
    float extent = 30;
    /// Region depth along the light, centered on #center, finite and positive.
    float depth = 100;
    /// Depth map edge in texels, nonzero. While the region is #enabled, VulkanRenderer::set_environment also
    /// limits it to the device; a disabled region keeps a 1x1 map whatever its value.
    std::uint32_t resolution = 2048;
    /// Receiver bias in normalized shadow depth, where 1 spans #depth; finite and nonnegative.
    float constant_bias = .0005F;
    /// Extra bias times `1 - max(dot(N, L), 0)` for the shading normal `N`, so it grows at grazing angles;
    /// finite and nonnegative.
    float slope_bias = .0015F;
};
/// Lighting settings other than the two lights, shared by SceneEnvironment and Environment.
///
/// validate_environment_settings() defines the accepted values. Fog, exposure and tone mapping do not apply
/// to UI.
struct EnvironmentSettings {
    /// Ambient light for normals facing +Y. Diffuse ambient light blends this and #ambient_ground by the
    /// normal's Y component, times albedo and `1 - metallic`. Finite and nonnegative, like every color here.
    Vec3 ambient_sky{.4F, .4F, .4F};
    /// Ambient light for normals facing -Y.
    Vec3 ambient_ground{.4F, .4F, .4F};
    /// Constant specular ambient light, times the surface's normal-incidence reflectance. Occlusion scales
    /// both ambient terms.
    Vec3 ambient_specular{.08F, .08F, .08F};
    /// Draws a gradient sky behind the scene instead of the fixed clear color, with a disc and glow toward
    /// Environment::sun scaled by its radiance. The sky is not fogged.
    bool sky = false;
    /// Sky color straight up, blending toward #sky_horizon as the view ray nears the horizon.
    Vec3 sky_zenith{.10F, .25F, .48F};
    /// Sky color at the horizon.
    Vec3 sky_horizon{.55F, .65F, .75F};
    /// Sky color below the horizon, reached where the view ray's Y component is -0.25 or lower.
    Vec3 sky_ground{.12F, .15F, .18F};
    /// Color that fog blends toward.
    Vec3 fog_color{.55F, .65F, .75F};
    /// Fog density per world unit, finite and nonnegative; zero disables fog. In perspective views a mesh
    /// surface keeps `exp(-fog_density * distance)` of its color, `distance` being from the eye, and takes the
    /// rest from #fog_color. Orthographic views are not fogged.
    float fog_density = 0;
    /// Multiplies linear scene color, including the sky and background, before tone mapping; finite and
    /// positive.
    float exposure = 1;
    /// Compresses each exposed channel `c` to `c / (1 + c)` before display encoding.
    bool tone_mapping = false;
    /// World shadow region of Environment::sun.
    DirectionalShadow shadow;
    /// Optional finer region, for example around a moving subject; move it by updating its center. Inside it,
    /// its result replaces the world region's.
    DirectionalShadow detail_shadow;
};
/// Complete lighting input of VulkanRenderer::set_environment; lighting_environment() resolves one from scene
/// components. The defaults give two lights, uniform ambient light and no sky, fog, tone mapping or shadows.
struct Environment : EnvironmentSettings {
    /// Key light: the only one that casts shadows, and the direction of the sky's sun disc and of both shadow
    /// regions.
    DirectionalLight sun;
    /// Fill light, without shadows.
    DirectionalLight fill{{.8F, .3F, -.5F}, {.314159265F, .314159265F, .314159265F}};
};
/// Throws `std::invalid_argument` unless every color in @p e is finite and nonnegative, `fog_density` is finite
/// and nonnegative, `exposure` is finite and positive, and both shadow regions, enabled or not, have finite
/// centers, finite positive `extent` and `depth`, nonzero `resolution` and finite nonnegative biases.
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
/// Validates @p e as validate_environment_settings() does, and requires each light to have finite, nonnegative
/// radiance and a direction whose squared length is finite and at least `1e-12`. Throws `std::invalid_argument`.
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
/// Column-major view-projection of the sun's EnvironmentSettings::detail_shadow region when @p detail is true,
/// otherwise of EnvironmentSettings::shadow, whether or not the region is enabled.
///
/// Maps the region's snapped box (see DirectionalShadow) to Vulkan clip space: Y down, depth 0 to 1 with 0 on
/// the side facing the light. Validates @p e with validate_environment() first. Throws `std::invalid_argument`
/// when the texel size or the matrix is not finite, and anima::MathError when the matrix cannot be inverted.
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
