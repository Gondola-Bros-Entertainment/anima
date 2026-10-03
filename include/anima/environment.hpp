#pragma once
#include <algorithm>
#include <anima/core/math.hpp>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

/// @file
/// Lighting environment values for anima::VulkanRenderer, with their validation and the sun's shadow projections.
/// Header-only; needs only `anima::core`.
///
/// Colors and radiance are linear RGB, and directions point from the surface toward the light. Validation
/// throws `std::invalid_argument`, or anima::MathError (derived from it) for a projection that cannot be inverted.

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
/// The sun's shadow cascades, which the renderer fits to the view every frame as fit_shadow_cascades() does.
///
/// They shade the view from its near plane to #distance, or to its far plane where that is nearer, by view depth:
/// distance along the view direction, from the eye in a perspective view and from the near plane in an orthographic
/// one. That range splits into #count cascades, and cascade `i` shades the view depths from split `i` to split
/// `i + 1`. In a perspective view whose near plane lies at depth `n`, with shadows ending at depth `f`, split `k` of
/// `N` lies at `s * n * (f / n)^(k / N) + (1 - s) * (n + (f - n) * k / N)`, where `s` is #logarithmic_split, so
/// nearer cascades shade shorter ranges; an orthographic view splits its range evenly. Over the last #blend share of
/// its range a cascade's shadow blends linearly into the next one's, and the last cascade's into no shadow, so
/// neither a seam between cascades nor the end of the shadows shows as a line.
///
/// Each cascade renders an orthographic square across the sun, `2 * radius` wide (ShadowCascade::radius), so a
/// texel spans `2 * radius / resolution`. The square holds a sphere around the cascade's slice of the view: the
/// view's frustum between the cascade's far split and the depth where the previous cascade starts blending into it.
/// The sphere's center lies on the line between the centers of the slice's near and far faces, at the point equally
/// far from the corners of both, or at the far face's center where that point would lie beyond it; its radius reaches
/// the farthest corner. The radius grows by 3 texels, so that the filter never reads past the square, and rounds up to
/// a multiple of `2^(floor(log2(radius)) - 10)`, a step of at most a tenth of a percent. A view's slices keep their
/// size while it turns or moves, so the radius stays constant, unless the float rounding of the view's matrix carries a
/// radius within about a ten-millionth of a multiple across it, and the square's center snaps to whole texels along
/// axes across the sun that depend only on its direction: moving the camera moves a cascade's texels by whole
/// texels, which leaves shadow edges where they lie. Depth spans the sphere, extended toward the sun over every caster
/// whose bounds reach into the square's column toward the sun, each end rounded outward to whole texels; casters
/// beyond the sphere cannot shadow what it holds.
///
/// Each cascade filters as DirectionalShadow describes, with biases in its own texels, and casts from every shadow
/// casting draw whose bounds reach into its square's column toward the sun, short of the far side of its sphere. A
/// disabled set is validated like an enabled one; only the device limit that VulkanRenderer::set_environment places on
/// #resolution is skipped while it is disabled.
struct ShadowCascades {
    /// Renders each cascade's casters into its depth map every frame. Disabled, the sun casts shadows only in
    /// EnvironmentSettings::detail_shadow, and the cascades keep only a 1x1 placeholder map.
    bool enabled = false;
    /// Number of cascades, from 1 to 4.
    std::uint32_t count = 4;
    /// View depth at which shadows end, finite, positive and at most 1,000,000,000.
    float distance = 100;
    /// Share of the logarithmic distribution in each split, from 0 (even splits) to 1 (logarithmic).
    float logarithmic_split = .75F;
    /// Share of each cascade's range over which it blends into the next, from 0 to 1.
    float blend = .1F;
    /// Depth map edge in texels of every cascade, at least 16. While the cascades are #enabled,
    /// VulkanRenderer::set_environment also limits it to the device; disabled, they keep a 1x1 map whatever its value.
    std::uint32_t resolution = 2048;
    /// Receiver bias along the sun, in texels of the cascade; finite and nonnegative.
    float constant_bias = 1;
    /// Extra bias in texels times `1 - max(dot(N, L), 0)` for the shading normal `N`, so it grows at grazing angles;
    /// finite and nonnegative.
    float slope_bias = 3;
};
/// An orthographic shadow region of Environment::sun fixed in world space: EnvironmentSettings::detail_shadow.
///
/// The region is a box around #center, `2 * extent` wide on both axes across the light and #depth deep
/// along it, so a texel spans `2 * extent / resolution`. The center snaps to whole texels across the light,
/// which reduces shimmer when a region follows a subject. Only casters inside the box cast. Inside the box the
/// region's result replaces the cascades' (EnvironmentSettings::shadow_cascades), blending into theirs over the
/// outer 4 percent of the box on each axis.
///
/// Filtering takes a bilinearly weighted 3x3 comparison kernel over 4x4 texels. Each texel is compared with the
/// nearer to the light of the receiver's own depth and the receiver plane's depth at that texel, less the bias. The
/// plane, found from screen-space position derivatives rather than shading normals, keeps a sloped receiver from
/// shadowing itself toward the light; it never reaches deeper than the receiver, where a crease that bends toward the
/// light would leave the surface in front of it. Nearly edge-on planes fall back to the bias alone.
///
/// A disabled region is validated like an enabled one: validate_environment_settings() checks every field and
/// detail_shadow_matrix() its projection. Only the device limit that VulkanRenderer::set_environment places on
/// #resolution is skipped while the region is disabled.
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
    /// Shadow cascades of Environment::sun, fitted to the view.
    ShadowCascades shadow_cascades;
    /// Optional finer region, for example around a moving subject; move it by updating its center. Inside it,
    /// its result replaces the cascades'.
    DirectionalShadow detail_shadow;
};
/// Complete lighting input of VulkanRenderer::set_environment; lighting_environment() resolves one from scene
/// components. The defaults give two lights, uniform ambient light and no sky, fog, tone mapping or shadows.
struct Environment : EnvironmentSettings {
    /// Key light: the only one that casts shadows, and the direction of the sky's sun disc, of the shadow cascades
    /// and of the detail region.
    DirectionalLight sun;
    /// Fill light, without shadows.
    DirectionalLight fill{{.8F, .3F, -.5F}, {.314159265F, .314159265F, .314159265F}};
};
/// Throws `std::invalid_argument` unless every color in @p e is finite and nonnegative, `fog_density` is finite
/// and nonnegative, `exposure` is finite and positive, the shadow cascades, enabled or not, meet the ranges that
/// ShadowCascades states, and the detail region, enabled or not, has a finite center, finite positive `extent`
/// and `depth`, nonzero `resolution` and finite nonnegative biases.
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
    constexpr float maximum_shadow_distance = 1e9F;
    const auto &c = e.shadow_cascades;
    // Negated comparisons also reject NaN.
    if (c.count < 1 || c.count > 4 || !(c.distance > 0 && c.distance <= maximum_shadow_distance) ||
        !(c.logarithmic_split >= 0 && c.logarithmic_split <= 1) || !(c.blend >= 0 && c.blend <= 1) ||
        c.resolution < 16 || !std::isfinite(c.constant_bias) || c.constant_bias < 0 || !std::isfinite(c.slope_bias) ||
        c.slope_bias < 0)
        throw std::invalid_argument("Invalid shadow cascades");
    const auto &s = e.detail_shadow;
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
/// A planet's atmosphere after Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique"
/// (EGSR 2020), over a spherical ground: Rayleigh and Mie scattering whose densities fall exponentially with altitude,
/// Mie absorption, and ozone absorption in a tent around an altitude. Lengths are in meters and coefficients per meter,
/// each at its medium's densest. The defaults are Earth's, as the paper's reference implementation sets them;
/// validate_atmosphere() defines the accepted values.
struct Atmosphere {
    /// Radius of the ground.
    float planet_radius = 6'360'000;
    /// Height of the atmosphere's top above the ground.
    float thickness = 100'000;
    /// Rayleigh scattering per channel at the ground. Rayleigh scattering absorbs nothing.
    Vec3 rayleigh_scattering{5.802e-6F, 13.558e-6F, 33.1e-6F};
    /// Altitude over which Rayleigh density falls by a factor of e.
    float rayleigh_scale_height = 8'000;
    /// Mie scattering per channel at the ground.
    Vec3 mie_scattering{3.996e-6F, 3.996e-6F, 3.996e-6F};
    /// Mie absorption per channel at the ground.
    Vec3 mie_absorption{4.44e-7F, 4.44e-7F, 4.44e-7F};
    /// Altitude over which Mie density falls by a factor of e.
    float mie_scale_height = 1'200;
    /// Ozone absorption per channel at #ozone_altitude, falling linearly to zero #ozone_width / 2 above and below it.
    Vec3 ozone_absorption{6.50e-7F, 1.881e-6F, 8.5e-8F};
    /// Altitude of the ozone layer's peak.
    float ozone_altitude = 25'000;
    /// Width of the ozone layer.
    float ozone_width = 30'000;
};
/// Throws `std::invalid_argument("Invalid atmosphere")` unless @p a's radius, thickness, scale heights and ozone
/// width are finite and positive, `planet_radius + thickness` is at most 1,000,000,000, every coefficient is finite
/// and nonnegative, and `ozone_altitude` is finite.
inline void validate_atmosphere(const Atmosphere &a) {
    const auto positive = [](float v) { return std::isfinite(v) && v > 0; };
    const auto coefficients = [](Vec3 c) {
        return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && c.x >= 0 && c.y >= 0 && c.z >= 0;
    };
    constexpr double maximum_radius = 1e9;
    if (!positive(a.planet_radius) || !positive(a.thickness) ||
        double(a.planet_radius) + a.thickness > maximum_radius || !positive(a.rayleigh_scale_height) ||
        !positive(a.mie_scale_height) || !positive(a.ozone_width) || !std::isfinite(a.ozone_altitude) ||
        !coefficients(a.rayleigh_scattering) || !coefficients(a.mie_scattering) || !coefficients(a.mie_absorption) ||
        !coefficients(a.ozone_absorption))
        throw std::invalid_argument("Invalid atmosphere");
}
/// The share of light, per channel, that crosses @p a from @p altitude meters above the ground to its top, along the
/// direction whose cosine with the zenith is @p cos_zenith: `exp(-t)`, where `t` integrates the extinction along the
/// straight path, Rayleigh scattering, Mie scattering and absorption and ozone absorption, each its coefficient times
/// its density. Zero where the path meets the ground, which a path below the horizon does from the ground; a path
/// along the horizon from the ground is open. The integral takes 1,024 midpoint steps in the square root of the
/// distance, which crowds them toward the start, in double precision. Throws `std::invalid_argument` as
/// validate_atmosphere() does, or with "Invalid atmosphere sample" unless @p altitude lies from 0 to the thickness and
/// @p cos_zenith from -1 to 1.
[[nodiscard]] inline Vec3 atmosphere_transmittance(const Atmosphere &a, double altitude, double cos_zenith) {
    validate_atmosphere(a);
    if (!(altitude >= 0 && altitude <= a.thickness) || !(cos_zenith >= -1 && cos_zenith <= 1))
        throw std::invalid_argument("Invalid atmosphere sample");
    const double ground = a.planet_radius, top = ground + double(a.thickness), r = ground + altitude, mu = cos_zenith;
    if (mu < 0 && r * r * (mu * mu - 1) + ground * ground >= 0)
        return {0, 0, 0};
    const double span = -r * mu + std::sqrt(std::max(r * r * (mu * mu - 1) + top * top, 0.0));
    // Midpoints in u, at t = span * u^2, weighted by dt/du = 2 * span * u.
    constexpr int steps = 1024;
    std::array<double, 3> depth{};
    for (int i = 0; i < steps; ++i) {
        const double u = (i + .5) / steps, t = span * u * u;
        const double height = std::sqrt(r * r + 2 * r * mu * t + t * t) - ground;
        const double rayleigh = std::exp(-height / a.rayleigh_scale_height),
                     mie = std::exp(-height / a.mie_scale_height),
                     ozone = std::max(0.0, 1 - std::abs(height - a.ozone_altitude) / (a.ozone_width / 2.0));
        const double weight = 2 * span * u / steps;
        const std::array extinction{
            a.rayleigh_scattering.x * rayleigh + (double(a.mie_scattering.x) + a.mie_absorption.x) * mie +
                a.ozone_absorption.x * ozone,
            a.rayleigh_scattering.y * rayleigh + (double(a.mie_scattering.y) + a.mie_absorption.y) * mie +
                a.ozone_absorption.y * ozone,
            a.rayleigh_scattering.z * rayleigh + (double(a.mie_scattering.z) + a.mie_absorption.z) * mie +
                a.ozone_absorption.z * ozone};
        for (std::size_t c = 0; c < depth.size(); ++c)
            depth[c] += extinction[c] * weight;
    }
    return {float(std::exp(-depth[0])), float(std::exp(-depth[1])), float(std::exp(-depth[2]))};
}
namespace detail {
/// Axes of a view along the sun with direction @p sun toward it: right, up and forward (away from the sun), in
/// double. They depend only on the sun's direction.
inline std::array<std::array<double, 3>, 3> sun_axes(Vec3 sun) {
    const auto light = normalized(sun);
    const std::array<double, 3> forward{-double(light.x), -double(light.y), -double(light.z)};
    const std::array<double, 3> hint = std::abs(forward[1]) > .99 ? std::array{1., 0., 0.} : std::array{0., 1., 0.};
    std::array<double, 3> right{forward[1] * hint[2] - forward[2] * hint[1],
                                forward[2] * hint[0] - forward[0] * hint[2],
                                forward[0] * hint[1] - forward[1] * hint[0]};
    const auto length = std::hypot(right[0], right[1], right[2]);
    for (auto &value : right)
        value /= length;
    const std::array<double, 3> up{right[1] * forward[2] - right[2] * forward[1],
                                   right[2] * forward[0] - right[0] * forward[2],
                                   right[0] * forward[1] - right[1] * forward[0]};
    return {right, up, forward};
}
/// Orthographic view-projection along the sun's @p axes (sun_axes()) over the square of half-width @p radius
/// around light-space position @p across (along right and up), with depth along forward from @p near_depth (0) to
/// @p far_depth (1); Vulkan clip space with Y down. Throws `std::invalid_argument` when an element is not finite as a
/// float.
inline Mat4 sun_projection(const std::array<std::array<double, 3>, 3> &axes, std::array<double, 2> across,
                           double radius, double near_depth, double far_depth) {
    const auto &[right, up, forward] = axes;
    const double range = far_depth - near_depth;
    Mat4 m{};
    for (int axis = 0; axis < 3; ++axis) {
        m[axis * 4] = float(right[axis] / radius);
        m[axis * 4 + 1] = float(-up[axis] / radius);
        m[axis * 4 + 2] = float(forward[axis] / range);
    }
    m[12] = float(-across[0] / radius);
    m[13] = float(across[1] / radius);
    m[14] = float(-near_depth / range);
    m[15] = 1;
    for (const auto value : m)
        if (!std::isfinite(value))
            throw std::invalid_argument("Shadow projection exceeds finite range");
    return m;
}
} // namespace detail
/// Column-major view-projection of the sun's EnvironmentSettings::detail_shadow region, whether or not it is enabled.
///
/// Maps the region's snapped box (see DirectionalShadow) to Vulkan clip space: Y down, depth 0 to 1 with 0 on
/// the side facing the light. Validates @p e with validate_environment() first. Throws `std::invalid_argument`
/// when the texel size or the matrix is not finite, and anima::MathError when the matrix cannot be inverted.
inline Mat4 detail_shadow_matrix(const Environment &e) {
    validate_environment(e);
    const auto &region = e.detail_shadow;
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
/// One of the sun's shadow cascades (ShadowCascades) fitted to a view by fit_shadow_cascades().
struct ShadowCascade {
    /// View depth where the cascade starts shading: the view's near plane for the first, the previous cascade's
    /// #end for the others.
    float begin{};
    /// View depth where it stops. Over the last ShadowCascades::blend share of the depths from #begin it blends into
    /// the next cascade, or the last one into no shadow.
    float end{};
    /// World-space center of its square, on whole texels across the sun.
    Vec3 center{};
    /// Half the square's width across the sun.
    float radius{};
    /// World size of one texel, `2 * radius / ShadowCascades::resolution`.
    float texel{};
    /// Column-major view-projection: the square to Vulkan clip X and Y, with Y down, and depth along the sun from 0
    /// at the side of the cascade's sphere facing the sun to 1 at its far side, both rounded outward to whole
    /// texels. The renderer extends depth toward the sun to its farthest caster that the square covers.
    Mat4 view_projection{};
};
namespace detail {
/// A cascade as fit_shadow_cascades() fits it, in double: its ShadowCascade fields and its sun-space position.
struct CascadeFit {
    double begin{}, end{}, radius{}, texel{};
    /// Center along the sun's right and up axes, in whole texels, and its depth along forward.
    std::array<double, 2> across{};
    double depth{};
};
/// The cascades that fit_cascades() fits to a view, and how it measures view depth.
struct CascadeFits {
    /// Cascades fitted, from nearest to farthest, which #cascades holds first.
    std::uint32_t count{};
    std::array<CascadeFit, 4> cascades{};
    /// The point that view depths are measured from, the eye or the near plane's center, and the unit view direction.
    std::array<double, 3> origin{}, forward{};
};
/// Fits @p e's shadow cascades to @p view_projection, as fit_shadow_cascades() documents, along @p axes
/// (sun_axes()).
inline CascadeFits fit_cascades(const Environment &e, const Mat4 &view_projection,
                                const std::array<std::array<double, 3>, 3> &axes) {
    CascadeFits result;
    using Vector = std::array<double, 3>;
    const auto add = [](Vector a, Vector b) { return Vector{a[0] + b[0], a[1] + b[1], a[2] + b[2]}; };
    const auto subtract = [](Vector a, Vector b) { return Vector{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
    const auto scale = [](Vector a, double s) { return Vector{a[0] * s, a[1] * s, a[2] * s}; };
    const auto dot3 = [](Vector a, Vector b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const auto cross3 = [](Vector a, Vector b) {
        return Vector{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    const auto length3 = [&](Vector a) { return std::sqrt(dot3(a, a)); };
    const auto &settings = e.shadow_cascades;
    const auto origin = view_origin(view_projection);
    const bool perspective = origin[3] != 0;
    // Rows of the view-projection: X, Y and Z coefficients, and the constant.
    std::array<Vector, 4> row{};
    std::array<double, 4> constant{};
    for (int r = 0; r < 4; ++r) {
        row[r] = {view_projection[r], view_projection[4 + r], view_projection[8 + r]};
        constant[r] = view_projection[12 + r];
    }
    constexpr std::array<std::array<double, 2>, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
    // The frustum's shape comes from the matrix's linear part alone, which does not depend on where the camera is,
    // so a moving camera's slices keep their size. A slice's corners at view depth d lie at reference + corner(c, d).
    std::array<Vector, 4> rays{};
    Vector reference{}, forward{};
    double start = 0, far_depth = std::numeric_limits<double>::infinity();
    if (perspective) {
        // The eye maps to (0, 0, k, 0), and reversed depth puts the near plane where clip Z equals W.
        reference = {origin[0], origin[1], origin[2]};
        const auto axis = subtract(row[3], row[2]);
        const auto rate = length3(axis);
        forward = scale(axis, 1 / rate);
        const auto k = dot3(row[2], reference) + constant[2];
        start = k / rate;
        // Clip Z reaches 0, the far plane, at this depth; an infinite projection never reaches it.
        if (const auto fall = dot3(row[2], forward); fall < 0)
            far_depth = -k / fall;
        for (std::size_t c = 0; c < corners.size(); ++c) {
            // The ray through a corner keeps clip X and Y at plus or minus clip W.
            auto ray =
                cross3(subtract(row[0], scale(row[3], corners[c][0])), subtract(row[1], scale(row[3], corners[c][1])));
            rays[c] = scale(ray, 1 / dot3(ray, forward));
        }
        if (!(start > 0))
            return result;
    } else {
        const auto inverted = inverse(view_projection);
        const auto column = [&](int c) { return Vector{inverted[c * 4], inverted[c * 4 + 1], inverted[c * 4 + 2]}; };
        // Reversed depth: from the near plane at clip Z 1 to the far plane at 0.
        reference = add(column(2), column(3));
        const auto depth = scale(column(2), -1);
        far_depth = length3(depth);
        forward = scale(depth, 1 / far_depth);
        for (std::size_t c = 0; c < corners.size(); ++c)
            rays[c] = add(scale(column(0), corners[c][0]), scale(column(1), corners[c][1]));
    }
    result.origin = reference;
    result.forward = forward;
    const double end = std::min(double(settings.distance), far_depth);
    if (!(end > start))
        return result;
    const auto count = settings.count;
    std::array<double, 5> splits{};
    for (std::uint32_t k = 0; k <= count; ++k) {
        const double share = double(k) / count, even = start + (end - start) * share;
        splits[k] = perspective ? settings.logarithmic_split * start * std::pow(end / start, share) +
                                      (1 - settings.logarithmic_split) * even
                                : even;
    }
    splits[0] = start;
    splits[count] = end;
    const auto at = [&](std::size_t c, double depth) {
        return perspective ? scale(rays[c], depth) : add(rays[c], scale(forward, depth));
    };
    const auto &[right, up, light_forward] = axes;
    const double resolution = settings.resolution;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto &cascade = result.cascades[i];
        cascade.begin = splits[i];
        cascade.end = splits[i + 1];
        const auto from = i ? splits[i] - settings.blend * (splits[i] - splits[i - 1]) : splits[i];
        std::array<Vector, 8> slice{};
        Vector near_center{}, far_center{};
        for (std::size_t c = 0; c < 4; ++c) {
            slice[c] = at(c, from);
            slice[c + 4] = at(c, cascade.end);
            near_center = add(near_center, scale(slice[c], .25));
            far_center = add(far_center, scale(slice[c + 4], .25));
        }
        double near_radius = 0, far_radius = 0;
        for (std::size_t c = 0; c < 4; ++c) {
            near_radius = std::max(near_radius, length3(subtract(slice[c], near_center)));
            far_radius = std::max(far_radius, length3(subtract(slice[c + 4], far_center)));
        }
        // The point between the faces' centers equally far from both faces' corners, on the far face when the far
        // face is the wider by enough that the point would lie beyond it.
        const auto between = subtract(far_center, near_center);
        const auto gap = dot3(between, between);
        const auto t = std::clamp((gap + far_radius * far_radius - near_radius * near_radius) / (2 * gap), 0., 1.);
        const auto offset = add(near_center, scale(between, t));
        double radius = 0;
        for (const auto &corner : slice)
            radius = std::max(radius, length3(subtract(corner, offset)));
        // 3 texels of margin keep the filter, whose 4x4 texels reach 2.5 texels from a sample, inside the square
        // after snapping moves the center by up to half a texel.
        constexpr double margin = 3;
        radius *= resolution / (resolution - 2 * margin);
        const auto step = std::ldexp(1., std::ilogb(radius) - 10);
        radius = std::ceil(radius / step) * step;
        cascade.radius = radius;
        cascade.texel = 2 * radius / resolution;
        const auto center = add(reference, offset);
        cascade.across = {std::round(dot3(right, center) / cascade.texel) * cascade.texel,
                          std::round(dot3(up, center) / cascade.texel) * cascade.texel};
        cascade.depth = dot3(light_forward, center);
    }
    result.count = count;
    return result;
}
} // namespace detail
/// Fits EnvironmentSettings::shadow_cascades of @p e to @p view_projection, a view as VulkanRenderer::set_view takes
/// it, whether or not the cascades are enabled, as the renderer fits them each frame (see ShadowCascades). Returns
/// ShadowCascades::count cascades from nearest to farthest, or none when a perspective view's near plane does not lie
/// in front of its eye, or the view's near plane lies at or beyond ShadowCascades::distance.
///
/// Validates @p e with validate_environment() first. Throws anima::MathError when @p view_projection is not finite or
/// cannot be inverted, as anima::inverse() does, and `std::invalid_argument` when a result is not finite as a
/// float.
[[nodiscard]] inline std::vector<ShadowCascade> fit_shadow_cascades(const Environment &e, const Mat4 &view_projection) {
    validate_environment(e);
    (void)inverse(view_projection);
    const auto axes = detail::sun_axes(e.sun.direction);
    const auto fits = detail::fit_cascades(e, view_projection, axes);
    const auto count = fits.count;
    std::vector<ShadowCascade> result;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto &fit = fits.cascades[i];
        ShadowCascade cascade;
        cascade.begin = float(fit.begin);
        cascade.end = float(fit.end);
        const auto &[right, up, forward] = axes;
        const auto component = [&](std::size_t axis) {
            return float(right[axis] * fit.across[0] + up[axis] * fit.across[1] + forward[axis] * fit.depth);
        };
        cascade.center = {component(0), component(1), component(2)};
        cascade.radius = float(fit.radius);
        cascade.texel = float(fit.texel);
        const auto near_depth = std::floor((fit.depth - fit.radius) / fit.texel) * fit.texel;
        const auto far_depth = std::ceil((fit.depth + fit.radius) / fit.texel) * fit.texel;
        cascade.view_projection = detail::sun_projection(axes, fit.across, fit.radius, near_depth, far_depth);
        for (const auto value : {cascade.begin, cascade.end, cascade.center.x, cascade.center.y, cascade.center.z,
                                 cascade.radius, cascade.texel})
            if (!std::isfinite(value))
                throw std::invalid_argument("Shadow cascades exceed finite range");
        result.push_back(cascade);
    }
    return result;
}
} // namespace anima
