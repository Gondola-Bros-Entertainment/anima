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
/// Colors, radiance and irradiance are linear RGB, and directions point from the surface toward the light. Lengths are
/// in world units; the atmosphere takes one world unit as one meter, so Atmosphere::ground_height and the eye's
/// altitude above it are in meters. Validation throws `std::invalid_argument`, or anima::MathError (derived from it)
/// for a projection that cannot be inverted; a rejected setting's message names its field as the
/// `anima.scene-environment.v4` payload spells it (add_lighting_component_codecs()).

namespace anima {
/// One directional light of an Environment.
///
/// Lit surfaces reflect the radiance `BRDF * irradiance * max(dot(N, L), 0)`, where `L` is the normalized #direction,
/// further scaled by shadow visibility for Environment::sun.
struct DirectionalLight {
    /// Direction from the surface toward the light, normalized when used. Its squared length must be finite
    /// and at least `1e-12`.
    Vec3 direction{-.6F, .9F, .8F};
    /// Finite, nonnegative linear RGB irradiance on a surface facing the light: a Lambertian surface of albedo `a`
    /// facing it reflects the radiance `a * irradiance / pi`.
    Vec3 irradiance{std::numbers::pi_v<float> / 2, std::numbers::pi_v<float> / 2, std::numbers::pi_v<float> / 2};
};
/// How far a shadow map's comparisons move each receiver toward the sun, in texels of the map: ShadowCascades::bias and
/// DirectionalShadow::bias.
///
/// The filter compares the map's depths with the receiver moved `(constant + slope * (1 - max(dot(N, L), 0))) * t`
/// toward the sun, for the shading normal `N`, the unit direction `L` toward the sun and the world size `t` of one of
/// the map's texels, so that the bias follows the map's resolution and coverage. Where the device stores shadow depth
/// in 16 bits, the renderer adds one of its 65,535 steps across the map's depth range, which the format cannot resolve.
struct ShadowBias {
    /// Bias in texels at every angle; finite and nonnegative.
    float constant = 1;
    /// Extra bias in texels times `1 - max(dot(N, L), 0)`, so that it grows at grazing angles; finite and nonnegative.
    float slope = 3;
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
/// Each cascade filters as DirectionalShadow describes, with #bias in its own texels, and casts from every shadow
/// casting draw whose bounds reach into its square's column toward the sun, short of the far side of its sphere. A
/// disabled set is validated like an enabled one; only the device limit that VulkanRenderer::set_environment places on
/// #resolution is skipped while it is disabled.
struct ShadowCascades {
    /// Most cascades a set may have, which the renderer's fixed cascade arrays hold.
    static constexpr std::uint32_t max_count = 4;
    /// Least #resolution, which leaves a cascade's square room for its 3 texels of margin on each side.
    static constexpr std::uint32_t min_resolution = 16;
    /// Renders each cascade's casters into its depth map every frame. Disabled, the sun casts shadows only in
    /// EnvironmentSettings::detail_shadow, and the cascades keep only a 1x1 placeholder map.
    bool enabled = false;
    /// Number of cascades, from 1 to #max_count.
    std::uint32_t count = max_count;
    /// View depth in world units at which shadows end, positive and at most 1,000,000,000.
    float distance = 100;
    /// Share of the logarithmic distribution in each split, from 0 (even splits) to 1 (logarithmic).
    float logarithmic_split = .75F;
    /// Share of each cascade's range over which it blends into the next, from 0 to 1.
    float blend = .1F;
    /// Depth map edge in texels of every cascade, at least #min_resolution. While the cascades are #enabled,
    /// VulkanRenderer::set_environment also limits it to the device; disabled, they keep a 1x1 map whatever its value.
    std::uint32_t resolution = 2048;
    /// Receiver bias in texels of each cascade.
    ShadowBias bias;
};
/// An orthographic shadow region of Environment::sun fixed in world space: EnvironmentSettings::detail_shadow.
///
/// The region is a box around #center, `2 * extent` wide on both axes across the light and #depth deep
/// along it, so a texel spans `2 * extent / resolution` world units. Its axes across the light are those of the shadow
/// cascades' squares (ShadowCascades), which depend only on the light's direction, and the center snaps to whole texels
/// along them, which reduces shimmer when a region follows a subject. Only casters inside the box cast. Inside the box
/// the region's result replaces the cascades' (EnvironmentSettings::shadow_cascades), blending into theirs over the
/// outer 4 percent of the box on each axis.
///
/// Filtering takes a bilinearly weighted 3x3 comparison kernel over 4x4 texels. Each texel is compared with the
/// nearer to the light of the receiver's own depth and the receiver plane's depth at that texel, less #bias. The
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
    /// Half the region's width across the light in world units, finite and positive.
    float extent = 30;
    /// Region depth along the light in world units, centered on #center, finite and positive.
    float depth = 100;
    /// Depth map edge in texels, nonzero. While the region is #enabled, VulkanRenderer::set_environment also
    /// limits it to the device; a disabled region keeps a 1x1 map whatever its value.
    std::uint32_t resolution = 2048;
    /// Receiver bias in texels of the region.
    ShadowBias bias{1.7F, 5.1F};
};
/// A planet's atmosphere after Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique"
/// (EGSR 2020), over a spherical ground: Rayleigh and Mie scattering whose densities fall exponentially with altitude,
/// Mie absorption, and ozone absorption in a tent around an altitude. Lengths are in meters, one per world unit, and
/// coefficients per meter, each at its medium's densest. The defaults are Earth's, as the paper's reference
/// implementation sets them; validate_atmosphere() defines the accepted values.
///
/// The sky's radiance along a view ray, per unit of the sun's irradiance above the atmosphere, integrates what the
/// media scatter toward the eye, dimmed by the transmittance back to it: the sun transmitted to each point, zero where
/// the ground blocks it, through Rayleigh's phase function and Cornette-Shanks' with #mie_anisotropy; plus the paper's
/// isotropic approximation of light scattered more than once, `L2 / (1 - f_ms)` per unit of scattering, with `L2` and
/// `f_ms` averaged over every direction from the point and `f_ms` limited to 0.99, so that a dense medium that loses
/// little light keeps the series finite; and, where the ray meets the ground, the ground's Lambertian reflection of
/// the transmitted sun. The renderer tabulates it, so it matches this integral to within the tables' resolution and
/// half-precision storage, which also limits the radiance to 65504 per unit of the sun's irradiance.
struct Atmosphere {
    /// Draws the sky from this atmosphere and lights the scene with the sun it transmits; see
    /// EnvironmentSettings::atmosphere.
    bool enabled = false;
    /// World height along +Y of the ground, altitude 0.
    float ground_height = 0;
    /// Radius of the ground.
    float planet_radius = 6'360'000;
    /// Height of the atmosphere's top above the ground, more than 2, so that the eye, kept 1 m inside the ground and
    /// the top, has room.
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
    /// Asymmetry `g` of Mie scattering's Cornette-Shanks phase function, greater than -1 and less than 1; positive
    /// values scatter forward, into a halo around the sun.
    float mie_anisotropy = .8F;
    /// Albedo of the ground per channel, from 0 to 1, which reflects the transmitted sun into the sky.
    Vec3 ground_albedo{.3F, .3F, .3F};
    /// Angular radius `r` of the sun's disc, in radians, greater than 0 and less than pi / 2. The sky adds the disc in
    /// the directions within `r` of the sun's: the radiance `irradiance / (2 * pi * (1 - cos(r)))` for the sun's
    /// irradiance above the atmosphere, times the transmittance along the view ray, which is zero into the ground.
    float sun_angular_radius = .004675F;
};
/// Validates @p a whether it is enabled or not. Checking the fields in their order, throws `std::invalid_argument` with
/// the message of the first rule broken: "Atmosphere ground_height must be finite", "Atmosphere planet_radius must be
/// finite and positive", "Atmosphere thickness must be finite and more than 2", "Atmosphere planet_radius plus
/// thickness must be at most 1,000,000,000", "Atmosphere rayleigh_scattering must be finite and nonnegative",
/// "Atmosphere rayleigh_scale_height must be finite and positive", "Atmosphere mie_scattering must be finite and
/// nonnegative", "Atmosphere mie_absorption must be finite and nonnegative", "Atmosphere mie_scale_height must be
/// finite and positive", "Atmosphere ozone_absorption must be finite and nonnegative", "Atmosphere ozone_altitude must
/// be finite", "Atmosphere ozone_width must be finite and positive", "Atmosphere mie_anisotropy must lie strictly
/// between -1 and 1", "Atmosphere ground_albedo must lie from 0 to 1 in every channel" and "Atmosphere
/// sun_angular_radius must lie strictly between 0 and pi / 2". A color or coefficient meets its rule only in every
/// channel.
inline void validate_atmosphere(const Atmosphere &a) {
    const auto positive = [](float v) { return std::isfinite(v) && v > 0; };
    const auto coefficients = [](Vec3 c) {
        return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && c.x >= 0 && c.y >= 0 && c.z >= 0;
    };
    const auto require = [](bool valid, const char *message) {
        if (!valid)
            throw std::invalid_argument(message);
    };
    constexpr double maximum_radius = 1e9;
    require(std::isfinite(a.ground_height), "Atmosphere ground_height must be finite");
    require(positive(a.planet_radius), "Atmosphere planet_radius must be finite and positive");
    require(std::isfinite(a.thickness) && a.thickness > 2, "Atmosphere thickness must be finite and more than 2");
    require(double(a.planet_radius) + a.thickness <= maximum_radius,
            "Atmosphere planet_radius plus thickness must be at most 1,000,000,000");
    require(coefficients(a.rayleigh_scattering), "Atmosphere rayleigh_scattering must be finite and nonnegative");
    require(positive(a.rayleigh_scale_height), "Atmosphere rayleigh_scale_height must be finite and positive");
    require(coefficients(a.mie_scattering), "Atmosphere mie_scattering must be finite and nonnegative");
    require(coefficients(a.mie_absorption), "Atmosphere mie_absorption must be finite and nonnegative");
    require(positive(a.mie_scale_height), "Atmosphere mie_scale_height must be finite and positive");
    require(coefficients(a.ozone_absorption), "Atmosphere ozone_absorption must be finite and nonnegative");
    require(std::isfinite(a.ozone_altitude), "Atmosphere ozone_altitude must be finite");
    require(positive(a.ozone_width), "Atmosphere ozone_width must be finite and positive");
    require(a.mie_anisotropy > -1 && a.mie_anisotropy < 1,
            "Atmosphere mie_anisotropy must lie strictly between -1 and 1");
    require(coefficients(a.ground_albedo) && a.ground_albedo.x <= 1 && a.ground_albedo.y <= 1 && a.ground_albedo.z <= 1,
            "Atmosphere ground_albedo must lie from 0 to 1 in every channel");
    require(a.sun_angular_radius > 0 && a.sun_angular_radius < std::numbers::pi_v<float> / 2,
            "Atmosphere sun_angular_radius must lie strictly between 0 and pi / 2");
}
/// Fog whose density falls exponentially with height: EnvironmentSettings::fog. validate_height_fog() defines the
/// accepted values.
///
/// In perspective views a mesh surface keeps the share `exp(-t)` of its color and takes the rest from the fog's light.
/// `t` integrates the density along the straight path of length `d` from the eye to the surface: with `k` the
/// #falloff, `y` the height of the path's lower end and `r` the height between its ends, `t = density * d * exp(-k *
/// (y - height)) * (1 - exp(-k * r)) / (k * r)`, where the last factor is 1 when `k * r` is zero, so that uniform fog
/// keeps `exp(-density * d)`. The fog's light is #color plus Environment::sun's irradiance as it reaches the ground,
/// atmosphere_sunlight(), times #sun_scattering times the Henyey-Greenstein phase function `(1 - g^2) / (4 * pi * (1 +
/// g^2 - 2 * g * c)^1.5)`, with `g` the #sun_anisotropy and `c` the cosine of the angle between the path, from the eye,
/// and the direction toward the sun. It depends on the path's direction alone, not on its length, and each of its
/// channels is capped at 65504, the largest half float that the scene target holds. Neither shadows nor the scene's
/// own light reach the fog. Orthographic views are not fogged.
struct HeightFog {
    /// Ambient light that the fog scatters toward the eye; finite and nonnegative.
    Vec3 color{.55F, .65F, .75F};
    /// Density per world unit at #height, finite and nonnegative; zero disables fog. The density at height `y` is
    /// `density * exp(-falloff * (y - height))`.
    float density = 0;
    /// Height along +Y at which the density is #density; finite.
    float height = 0;
    /// Rate per world unit at which the density falls with height above #height, and rises below it; finite and
    /// nonnegative. Zero makes the density uniform.
    float falloff = 0;
    /// Share of the sun as it reaches the ground, atmosphere_sunlight(), per channel, that the fog scatters toward the
    /// eye, weighted by the phase function; finite and nonnegative. Zero scatters no sunlight.
    Vec3 sun_scattering{0, 0, 0};
    /// Asymmetry `g` of the phase function, greater than -1 and less than 1. Positive values scatter sunlight forward,
    /// so the fog brightens toward the sun; zero scatters it evenly.
    float sun_anisotropy = .6F;
    /// Distance in world units along each view ray at which fog applies to the sky and to
    /// EnvironmentSettings::background, from 0 to 1,000,000,000. Geometry at this distance meets the sky or background
    /// behind it in the same fog, so a horizon of distant terrain fades into it. Zero leaves them unfogged.
    float sky_distance = 0;
};
/// Validates @p fog. Checking the fields in their order, throws `std::invalid_argument` with the message of the first
/// rule broken: "Fog color must be finite nonnegative linear RGB", "Fog density must be finite and nonnegative", "Fog
/// height must be finite", "Fog falloff must be finite and nonnegative", "Fog sun_scattering must be finite and
/// nonnegative", "Fog sun_anisotropy must lie strictly between -1 and 1" and "Fog sky_distance must lie from 0 to
/// 1,000,000,000". A color meets its rule only in every channel.
inline void validate_height_fog(const HeightFog &fog) {
    const auto color = [](Vec3 c) {
        return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && c.x >= 0 && c.y >= 0 && c.z >= 0;
    };
    const auto require = [](bool valid, const char *message) {
        if (!valid)
            throw std::invalid_argument(message);
    };
    // Bounding the distance keeps its products with the density and falloff within float range. The comparisons
    // reject NaN.
    constexpr float maximum_distance = 1e9F;
    require(color(fog.color), "Fog color must be finite nonnegative linear RGB");
    require(std::isfinite(fog.density) && fog.density >= 0, "Fog density must be finite and nonnegative");
    require(std::isfinite(fog.height), "Fog height must be finite");
    require(std::isfinite(fog.falloff) && fog.falloff >= 0, "Fog falloff must be finite and nonnegative");
    require(color(fog.sun_scattering), "Fog sun_scattering must be finite and nonnegative");
    require(fog.sun_anisotropy > -1 && fog.sun_anisotropy < 1, "Fog sun_anisotropy must lie strictly between -1 and 1");
    require(fog.sky_distance >= 0 && fog.sky_distance <= maximum_distance,
            "Fog sky_distance must lie from 0 to 1,000,000,000");
}
/// A curve that compresses exposed linear scene color before display encoding: EnvironmentSettings::tone_mapping. The
/// `anima.scene-environment.v4` payload stores it as its enumerator's name.
enum class ToneMapping : std::uint8_t {
    /// Applies no curve.
    none,
    /// Compresses each exposed channel `c` to `c / (1 + c)`.
    reinhard,
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
    /// The planet's atmosphere. While it is enabled, the renderer draws the sky from it behind the scene instead of
    /// #background, with the sun's disc, and lights surfaces and fog with the sun as it reaches the ground,
    /// atmosphere_sunlight(); the sky itself scatters the sun's irradiance above the atmosphere. The sky is seen from
    /// the eye's altitude above Atmosphere::ground_height, kept between 1 m and 1 m below the atmosphere's top, with
    /// the planet's center straight below the eye, so that +Y is up wherever the eye moves; an orthographic view sees
    /// it from 1 m above the ground. VulkanRenderer keeps the altitude that it last took from the eye until the eye's
    /// altitude differs from it by more than the larger of 1 mm and 8 float epsilons (about 1e-6) times the eye's
    /// distance from the world's origin, or until a thinner atmosphere's top falls to less than 1 m above it, then
    /// takes the eye's again: VulkanRenderer::set_view recovers the eye from a float view-projection, whose rounding
    /// moves the eye when the camera only turns. In perspective views fog applies to the sky as to a surface
    /// HeightFog::sky_distance along each view ray. Disabled, #background shows behind the scene and the sun keeps its
    /// irradiance.
    Atmosphere atmosphere;
    /// Linear radiance behind the scene while #atmosphere is disabled, wherever no mesh draws; the sky replaces it
    /// while the atmosphere is enabled. Finite, nonnegative and at most 65504 in each channel, the largest half float
    /// that the scene target holds. In perspective views fog applies to it as to a surface HeightFog::sky_distance
    /// along each view ray, as it does to the sky, and #exposure and #tone_mapping apply to it as to the scene.
    Vec3 background{.018F, .027F, .041F};
    /// Height fog over the scene, the sky and #background.
    HeightFog fog;
    /// Multiplies linear scene color, including the sky and background, before tone mapping; finite and
    /// positive.
    float exposure = 1;
    /// The curve that compresses exposed scene color before display encoding.
    ToneMapping tone_mapping = ToneMapping::none;
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
// The validation messages below state these limits.
static_assert(ShadowCascades::max_count == 4 && ShadowCascades::min_resolution == 16);
/// Validates @p e, its shadow maps whether they are enabled or not. Checking the fields in their order, throws
/// `std::invalid_argument` with the message of the first rule broken: "Environment ambient_sky must be finite
/// nonnegative linear RGB", and so for `ambient_ground` and `ambient_specular`; validate_atmosphere()'s messages;
/// "Environment background must be finite nonnegative linear RGB of at most 65504"; validate_height_fog()'s messages;
/// "Environment exposure must be finite and positive"; "Unknown tone mapping" unless `tone_mapping` is a ToneMapping
/// enumerator; "Shadow cascade count must be from 1 to 4", "Shadow cascade distance must be positive and at most
/// 1,000,000,000", "Shadow cascade logarithmic_split must lie from 0 to 1", "Shadow cascade blend must lie from 0 to
/// 1", "Shadow cascade resolution must be at least 16", "Shadow cascade bias constant must be finite and nonnegative"
/// and "Shadow cascade bias slope must be finite and nonnegative"; and "Detail shadow center must be finite", "Detail
/// shadow extent must be finite and positive", "Detail shadow depth must be finite and positive", "Detail shadow
/// resolution must be nonzero", "Detail shadow bias constant must be finite and nonnegative" and "Detail shadow bias
/// slope must be finite and nonnegative". A color or center meets its rule only in every channel or axis.
inline void validate_environment_settings(const EnvironmentSettings &e) {
    const auto color = [](Vec3 c) {
        return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && c.x >= 0 && c.y >= 0 && c.z >= 0;
    };
    const auto require = [](bool valid, const char *message) {
        if (!valid)
            throw std::invalid_argument(message);
    };
    const auto nonnegative = [](float v) { return std::isfinite(v) && v >= 0; };
    const auto positive = [](float v) { return std::isfinite(v) && v > 0; };
    require(color(e.ambient_sky), "Environment ambient_sky must be finite nonnegative linear RGB");
    require(color(e.ambient_ground), "Environment ambient_ground must be finite nonnegative linear RGB");
    require(color(e.ambient_specular), "Environment ambient_specular must be finite nonnegative linear RGB");
    validate_atmosphere(e.atmosphere);
    // The largest half float, which the scene target clears to at most.
    constexpr float maximum_half_float = 65504;
    const auto &b = e.background;
    require(color(b) && b.x <= maximum_half_float && b.y <= maximum_half_float && b.z <= maximum_half_float,
            "Environment background must be finite nonnegative linear RGB of at most 65504");
    validate_height_fog(e.fog);
    require(positive(e.exposure), "Environment exposure must be finite and positive");
    require(e.tone_mapping == ToneMapping::none || e.tone_mapping == ToneMapping::reinhard, "Unknown tone mapping");
    constexpr float maximum_distance = 1e9F;
    const auto &c = e.shadow_cascades;
    require(c.count >= 1 && c.count <= ShadowCascades::max_count, "Shadow cascade count must be from 1 to 4");
    require(c.distance > 0 && c.distance <= maximum_distance,
            "Shadow cascade distance must be positive and at most 1,000,000,000");
    require(c.logarithmic_split >= 0 && c.logarithmic_split <= 1,
            "Shadow cascade logarithmic_split must lie from 0 to 1");
    require(c.blend >= 0 && c.blend <= 1, "Shadow cascade blend must lie from 0 to 1");
    require(c.resolution >= ShadowCascades::min_resolution, "Shadow cascade resolution must be at least 16");
    require(nonnegative(c.bias.constant), "Shadow cascade bias constant must be finite and nonnegative");
    require(nonnegative(c.bias.slope), "Shadow cascade bias slope must be finite and nonnegative");
    const auto &s = e.detail_shadow;
    require(std::isfinite(s.center.x) && std::isfinite(s.center.y) && std::isfinite(s.center.z),
            "Detail shadow center must be finite");
    require(positive(s.extent), "Detail shadow extent must be finite and positive");
    require(positive(s.depth), "Detail shadow depth must be finite and positive");
    require(s.resolution != 0, "Detail shadow resolution must be nonzero");
    require(nonnegative(s.bias.constant), "Detail shadow bias constant must be finite and nonnegative");
    require(nonnegative(s.bias.slope), "Detail shadow bias slope must be finite and nonnegative");
}
/// Validates @p e as validate_environment_settings() does, then Environment::sun and then Environment::fill, throwing
/// `std::invalid_argument` with "Environment sun direction must have a finite squared length of at least 1e-12" or
/// "Environment sun irradiance must be finite nonnegative linear RGB", or the same of the fill light.
inline void validate_environment(const Environment &e) {
    validate_environment_settings(e);
    const auto light = [](const DirectionalLight &l, const char *direction, const char *irradiance) {
        const auto square = dot(l.direction, l.direction);
        if (!std::isfinite(square) || square < 1e-12F)
            throw std::invalid_argument(direction);
        const auto c = l.irradiance;
        if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || c.x < 0 || c.y < 0 || c.z < 0)
            throw std::invalid_argument(irradiance);
    };
    light(e.sun, "Environment sun direction must have a finite squared length of at least 1e-12",
          "Environment sun irradiance must be finite nonnegative linear RGB");
    light(e.fill, "Environment fill direction must have a finite squared length of at least 1e-12",
          "Environment fill irradiance must be finite nonnegative linear RGB");
}
namespace detail {
/// atmosphere_transmittance() of an atmosphere and a sample that it accepts, without validating them.
[[nodiscard]] inline Vec3 atmosphere_transmittance_of(const Atmosphere &a, double altitude, double cos_zenith) {
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
/// atmosphere_sunlight() of an environment that validate_environment() accepts, without validating it.
[[nodiscard]] inline Vec3 atmosphere_sunlight_of(const Environment &e) {
    const auto &a = e.atmosphere;
    if (!a.enabled)
        return e.sun.irradiance;
    const auto toward = normalized(e.sun.direction);
    const double elevation = std::asin(std::clamp(double(toward.y), -1.0, 1.0)), radius = a.sun_angular_radius;
    const double share = std::clamp((elevation + radius) / (2 * radius), 0.0, 1.0);
    if (share <= 0)
        return {0, 0, 0};
    const auto through = atmosphere_transmittance_of(a, 0, std::max(double(toward.y), 0.0));
    return {float(double(e.sun.irradiance.x) * through.x * share),
            float(double(e.sun.irradiance.y) * through.y * share),
            float(double(e.sun.irradiance.z) * through.z * share)};
}
} // namespace detail
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
    return detail::atmosphere_transmittance_of(a, altitude, cos_zenith);
}
/// The irradiance of @p e's sun as it reaches the ground. With the atmosphere enabled, Environment::sun's irradiance is
/// the light above the atmosphere, and this is that times atmosphere_transmittance() from the ground toward the sun,
/// along the horizon once the sun is below it, times the share of its disc above the horizon, `clamp((elevation +
/// r) / (2 * r), 0, 1)` for the sun's elevation and angular radius `r`; so the sun dims and reddens toward the horizon
/// and fades out across it. With the atmosphere disabled it is the sun's irradiance. Throws `std::invalid_argument` as
/// validate_environment() does.
[[nodiscard]] inline Vec3 atmosphere_sunlight(const Environment &e) {
    validate_environment(e);
    return detail::atmosphere_sunlight_of(e);
}
namespace detail {
/// The magnitude of the Y of the sun's unit direction above which sun_axes() crosses that direction with the world's X
/// axis instead of its Y axis, whose cross product with a sun that near vertical is too short to point precisely.
inline constexpr double vertical_sun_cosine = .99;
/// Axes of a view along the sun with direction @p sun toward it: right, up and forward (away from the sun), in
/// double. They depend only on the sun's direction.
inline std::array<std::array<double, 3>, 3> sun_axes(Vec3 sun) {
    const auto light = normalized(sun);
    const std::array<double, 3> forward{-double(light.x), -double(light.y), -double(light.z)};
    const std::array<double, 3> hint =
        std::abs(forward[1]) > vertical_sun_cosine ? std::array{1., 0., 0.} : std::array{0., 1., 0.};
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
/// detail_shadow_matrix() of an environment that validate_environment() accepts, without validating it again; it
/// throws as detail_shadow_matrix() states.
[[nodiscard]] inline Mat4 detail_shadow_matrix_of(const Environment &e) {
    const auto &region = e.detail_shadow;
    if (const auto texel = 2.F * region.extent / static_cast<float>(region.resolution);
        !std::isfinite(texel) || texel <= 0)
        throw std::invalid_argument("Shadow texel size exceeds finite range");
    const auto axes = sun_axes(e.sun.direction);
    const auto &[right, up, forward] = axes;
    const std::array<double, 3> center{region.center.x, region.center.y, region.center.z};
    const auto along = [&](const std::array<double, 3> &axis) {
        return axis[0] * center[0] + axis[1] * center[1] + axis[2] * center[2];
    };
    // The center snaps to whole texels across the sun, which keeps a following region's texels in place.
    const double texel = 2.0 * region.extent / region.resolution;
    const std::array across{std::round(along(right) / texel) * texel, std::round(along(up) / texel) * texel};
    const double depth = along(forward), half_depth = region.depth / 2.0;
    const auto matrix = sun_projection(axes, across, region.extent, depth - half_depth, depth + half_depth);
    (void)inverse(matrix);
    return matrix;
}
} // namespace detail
/// Column-major view-projection of the sun's EnvironmentSettings::detail_shadow region, whether or not it is enabled.
///
/// Maps the region's snapped box (see DirectionalShadow) to Vulkan clip space: Y down, depth 0 to 1 with 0 on
/// the side facing the light. Validates @p e with validate_environment() first. Throws `std::invalid_argument`
/// with "Shadow texel size exceeds finite range" unless the texel size, `2 * extent / resolution` computed in float,
/// is finite and positive, and with "Shadow projection exceeds finite range" when an element of the matrix is not
/// finite as a float: among others when the depth is so thin that its reciprocal overflows float, or so thin beside
/// the center's distance along the light that the box's near and far depths, formed from that distance in double,
/// round to the same value; and anima::MathError when the matrix cannot be inverted.
[[nodiscard]] inline Mat4 detail_shadow_matrix(const Environment &e) {
    validate_environment(e);
    return detail::detail_shadow_matrix_of(e);
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
    /// Half the square's width across the sun, in world units.
    float radius{};
    /// World size of one texel in world units, `2 * radius / ShadowCascades::resolution`.
    float texel{};
    /// Column-major view-projection: the square to Vulkan clip X and Y, with Y down, and depth along the sun from 0
    /// at the side of the cascade's sphere facing the sun to 1 at its far side, both rounded outward to whole
    /// texels. The renderer extends depth toward the sun to its farthest caster that the square covers.
    Mat4 view_projection{};
};
namespace detail {
/// Texels by which each cascade's square reaches past its sphere on every side: the filter, whose 4x4 texels reach 2.5
/// texels from a sample, stays inside the square after snapping moves the center by up to half a texel.
inline constexpr double cascade_margin_texels = 3;
static_assert(ShadowCascades::min_resolution > 2 * cascade_margin_texels,
              "A cascade's square must hold more than its margins");
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
    std::array<CascadeFit, ShadowCascades::max_count> cascades{};
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
    std::array<double, ShadowCascades::max_count + 1> splits{};
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
        radius *= resolution / (resolution - 2 * cascade_margin_texels);
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
