#pragma once
#include <anima/environment.hpp>
#include <anima/prefab.hpp>

namespace anima {
class SceneSet;

/// Directional illumination with linear RGB radiance. Light travels along the
/// object's local -Z; its world +Z supplies Environment's surface-to-source
/// direction. Positive scale is ignored; singular/sheared/reflected poses reject.
class DirectionalLightComponent {
  public:
    explicit DirectionalLightComponent(Vec3 radiance = {1.570796327F, 1.570796327F, 1.570796327F});
    [[nodiscard]] Vec3 radiance() const { return radiance_; }
    /// Finite, nonnegative RGB; zero explicitly produces no direct illumination.
    void set_radiance(Vec3 radiance);

  private:
    Vec3 radiance_;
};

/// Explicit selection for the default renderer's two directional slots. Each
/// link selects its object's current DirectionalLightComponent attachment.
/// Null links can persist as unconfigured state but cannot resolve an environment.
class SceneEnvironment {
  public:
    explicit SceneEnvironment(EnvironmentSettings settings = {});
    [[nodiscard]] const EnvironmentSettings &settings() const { return settings_; }
    /// Validate all settings before replacing the accepted configuration.
    void configure(EnvironmentSettings settings);
    GameObject sun, fill;

  private:
    EnvironmentSettings settings_;
};

/// Resolve exactly one active SceneEnvironment and its two active light links
/// within idle scenes after updates. No callbacks, renderer mutation or implicit
/// SceneSet::active selection. Missing/ambiguous/stale/foreign selection rejects.
/// Sun controls the existing sky disc and both shadow regions. Geometry and
/// camera selection remain independent. Device shadow limits are renderer checks.
[[nodiscard]] Environment lighting_environment(Scene &scene);
[[nodiscard]] Environment lighting_environment(SceneSet &scenes);

/// Strict v1 light/settings/link codecs inside the current scene/prefab v3 format.
/// ObjectReferences remaps each prefab copy; cross-document links reject.
void add_lighting_component_codecs(ComponentCodecs &codecs);
} // namespace anima
