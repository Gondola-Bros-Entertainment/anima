#pragma once
#include <anima/environment.hpp>
#include <anima/prefab.hpp>
#include <numbers>

/// @file
/// Scene components that select the directional lights and environment settings of anima::VulkanRenderer,
/// with their persistence codecs. Part of the `anima::assets` target; needs no SDL, Vulkan or display.
///
/// Invalid arguments throw `std::invalid_argument`, or anima::MathError (derived from it) for a shadow
/// projection that cannot be inverted, unless a member states otherwise.

namespace anima {
class SceneSet;

/// Component that makes its GameObject a directional light.
///
/// Light travels along the object's local -Z axis, so its +Z axis in world space is the surface-to-light
/// direction that Environment expects: an identity transform gives `{0, 0, 1}`. The orientation includes
/// parents; translation and positive scale are ignored. When lighting_environment() resolves the light, the
/// world axes must each be at least `0.0001` long, orthogonal within `0.0001` once normalized, and
/// right-handed. That rejects shear, singular axes and reflections, including shear from a rotated child under
/// a nonuniformly scaled parent, but only for resolution, not for scene transforms in general. The component
/// has no sun or fill role; SceneEnvironment assigns it.
class DirectionalLightComponent {
  public:
    /// Throws `std::invalid_argument` for an invalid @p radiance; see set_radiance().
    explicit DirectionalLightComponent(Vec3 radiance = {std::numbers::pi_v<float> / 2, std::numbers::pi_v<float> / 2,
                                                        std::numbers::pi_v<float> / 2});
    /// Linear RGB radiance, as DirectionalLight::radiance.
    [[nodiscard]] Vec3 radiance() const { return radiance_; }
    /// Sets finite, nonnegative linear RGB radiance, or throws `std::invalid_argument` and keeps the previous
    /// value. Zero gives no direct light, but the light must still resolve, and it disables neither the sky
    /// nor the shadow regions.
    void set_radiance(Vec3 radiance);

  private:
    Vec3 radiance_;
};

/// Component that selects the renderer's environment settings and its sun and fill lights.
///
/// lighting_environment() requires exactly one active SceneEnvironment among its scenes. The links are
/// GameObject handles: removing the light component makes resolution fail until one is added to the same
/// object again, and destroying the object or unloading or replacing its scene invalidates the link for good,
/// even if a later object has the same name or key. Shadow region centers are world coordinates; moving
/// this object does not move them.
class SceneEnvironment {
  public:
    /// Throws `std::invalid_argument` for invalid @p settings; see configure().
    explicit SceneEnvironment(EnvironmentSettings settings = {});
    /// Accepted settings.
    [[nodiscard]] const EnvironmentSettings &settings() const { return settings_; }
    /// Replaces the settings after validating all of them with validate_environment_settings(). Invalid
    /// settings throw `std::invalid_argument` and leave the accepted ones unchanged.
    void configure(EnvironmentSettings settings);
    /// Object whose DirectionalLightComponent supplies Environment::sun. Null leaves the environment
    /// unconfigured; it can persist but not resolve.
    GameObject sun;
    /// Object whose DirectionalLightComponent supplies Environment::fill, under the rules of #sun; it may be
    /// the same object.
    GameObject fill;

  private:
    EnvironmentSettings settings_;
};

/// Resolves the Environment for VulkanRenderer::set_environment from the one active SceneEnvironment in
/// @p scene.
///
/// Both links must name live objects of @p scene with an active DirectionalLightComponent and a valid
/// orientation (see DirectionalLightComponent); other lights may stay active without being selected.
/// Disabled environments and environments under inactive parents do not count. The result is validated with
/// validate_environment() and both shadow projections with directional_shadow_matrix(), including disabled
/// regions; VulkanRenderer::set_environment later checks enabled regions against device limits. Throws
/// `std::invalid_argument` for a missing, ambiguous, null, stale, foreign or inactive selection and for
/// invalid values, and `std::logic_error` while @p scene is updating, under construction or no longer live.
///
/// Read-only: runs no component hooks, advances no time, keeps no reference and changes no renderer. Resolve
/// after scene updates; geometry and camera selection are separate.
[[nodiscard]] Environment lighting_environment(Scene &scene);
/// Resolves as lighting_environment(Scene &) does across every scene of @p scenes, so links may cross scenes;
/// SceneSet::active() plays no part. Also throws `std::logic_error` while the set is changing.
[[nodiscard]] Environment lighting_environment(SceneSet &scenes);

/// Registers the `anima.directional-light.v1` and `anima.scene-environment.v1` component codecs together,
/// or neither: throws `std::invalid_argument` if @p codecs already has either type or key.
///
/// A directional-light payload is a JSON object with exactly `radiance`, three numbers. A scene-environment
/// payload has exactly `sun`, `fill` and `settings`. The links are object-key strings resolved through the
/// document's ObjectReferences, with `"0"` for a null link. They may point anywhere in the same document, and
/// each prefab instance maps them to its own objects; a link to a stale object or outside the captured
/// objects fails to capture, and a key the document lacks fails to decode.
///
/// `settings` has exactly the EnvironmentSettings field names. Colors and centers are three numbers, flags
/// are booleans, and each shadow region has exactly `enabled`, `center`, `extent`, `depth`, `resolution` (an
/// integer from 1 to 4,294,967,295), `constant_bias` and `slope_bias`. Numbers must be finite, and payloads
/// are at most 64 KiB. Unknown, missing or duplicate fields, wrong types and invalid values are rejected. The
/// scene or prefab stores transforms and enabled state. A missing or inactive light is not a decoding error;
/// lighting_environment() rejects it later.
void add_lighting_component_codecs(ComponentCodecs &codecs);
} // namespace anima
