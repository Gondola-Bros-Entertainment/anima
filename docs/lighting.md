# Scene directional lighting and environments

`<anima/lighting.hpp>` supplies `DirectionalLightComponent`, `SceneEnvironment`
and `lighting_environment` through `anima::assets`. They use production scene
objects and components without requiring SDL, Vulkan, physics or a display.
`<anima/environment.hpp>` supplies the backend-independent `EnvironmentSettings`,
`Environment`, `DirectionalLight` and `DirectionalShadow` values and validation.

```cpp
anima::Scene scene;
auto sun = scene.create("Sun"), fill = scene.create("Fill");
sun.add_component<anima::DirectionalLightComponent>(anima::Vec3{2, 1.8F, 1.5F});
fill.add_component<anima::DirectionalLightComponent>(anima::Vec3{});
anima::EnvironmentSettings settings;
settings.sky = true;
settings.exposure = 1.2F;
auto selected = scene.create("Environment").add_component<anima::SceneEnvironment>(settings);
selected->sun = sun;
selected->fill = fill;

// After scene updates; geometry and camera selection are separate operations.
renderer.set_environment(anima::lighting_environment(scene));
```

A directional light emits along its object's local **-Z**. Its world **+Z** is the
surface-to-source direction required by `Environment`; identity therefore gives
direction `{0, 0, 1}`. World orientation includes parents, while translation does
not affect illumination. Positive nonuniform scale is ignored. Axes must have
lengths of at least 0.0001, be orthogonal within 0.0001 after normalization and
form a right-handed basis. Shear, singular axes and reflections reject, including
shear caused by a rotated child under a nonuniformly scaled parent. These pose
restrictions apply at lighting resolution, not to general scene transforms.

Radiance is finite nonnegative linear RGB. `set_radiance` validates all channels
before publication. Zero radiance explicitly suppresses direct illumination;
the selected light must still exist, be active and have a valid pose. A light
component has no sun/fill role; the environment's explicit links assign the
default renderer's two slots. Both links may deliberately address the same light.
The sun slot also supplies the gradient sky's disc and both directional shadow
regions. Setting its radiance to zero does not disable the independently enabled
sky or shadow features.

`SceneEnvironment::configure` validates all `EnvironmentSettings` before changing
accepted state. Settings cover hemispherical ambient, ambient specular, the sky
gradient, fog, exposure, tone mapping and the two shadow regions described in
[materials and lighting](material-lighting.md). Shadow centers are explicit world
coordinates; moving the environment object does not move them. Applications own
appearance, shadow-region tracking, quality policy and transitions.

`lighting_environment(Scene|SceneSet)` requires exactly one active
`SceneEnvironment` across the supplied selection, with both links identifying
live objects that have active `DirectionalLightComponent` attachments in that
selection. Missing, inactive, null, stale, foreign or ambiguous selection rejects.
Other lights can remain active without being selected. Disabled environments and
environments under inactive parents do not participate. `SceneSet::active` never
chooses lighting. Cross-scene runtime links work in a set; resolving the standalone
environment scene rejects links to other scenes.

A link selects an object's current light attachment. Removing that attachment
causes resolution to fail; replacing it on the same live object allows resolution
again. Destroying the object or unloading/replacing its scene permanently
invalidates the link. Neither names nor persistent keys repair a stale link.

Resolve only on idle scenes after updates, subject to the same callback,
construction and SceneSet mutation restrictions as other scene drivers. Resolution
is read-only: it runs no hooks, advances no time, retains no scene and changes no
renderer. It also validates both shadow projections, including disabled regions,
because the renderer prepares both. Device-specific shadow limits and renderer
state are still checked by `set_environment`. A thrown resolution prevents that
renderer call and leaves its previously accepted environment intact. Applications
decide whether to repair the selection, skip drawing or retain the previous view.

Renderer geometry selection (`set_scenes`), [camera selection](cameras.md) and
lighting selection are independent and should be refreshed explicitly after
transitions. This API connects authored scenes to the existing two directional
lights; it does not add point/spot lights, additional light slots, physical
atmosphere, local shadow atlases, baked lighting or renderer extensibility.

## Persistence

```cpp
anima::ComponentCodecs codecs;
anima::add_lighting_component_codecs(codecs);
auto document = anima::serialize_scene(scene, {}, codecs);
auto restored = anima::load_scene(document, {}, codecs);
renderer.set_environment(anima::lighting_environment(*restored));
```

The strict `anima.directional-light.v1` payload contains exactly `radiance`, an
array of three numbers. `anima.scene-environment.v1` contains exactly `sun`,
`fill` and `settings`. Both links are canonical object-key strings resolved through
`ObjectReferences`; `"0"` represents a persistable but unconfigured null link.

The settings object contains exactly `ambient_sky`, `ambient_ground`,
`ambient_specular`, `sky`, `sky_zenith`, `sky_horizon`, `sky_ground`, `fog_color`,
`fog_density`, `exposure`, `tone_mapping`, `shadow` and `detail_shadow`. Colors and
centers are three-number arrays; flags are booleans. Each shadow object contains
exactly `enabled`, `center`, `extent`, `depth`, `resolution`, `constant_bias` and
`slope_bias`. Resolution must be an integer from 1 through 4,294,967,295, followed
by device-capability checks when supplying it to a renderer. Other scalar and
color domains are the existing environment validation contract.

Codecs use only current scene/prefab **v3** documents. Forward/backward and
cross-root links resolve against the complete loaded object graph, and every
prefab copy remaps its own internal light links. Missing light attachments and
inactive selections remain authored state and fail at resolution, not loading.
Stale, foreign, out-of-prefab and unresolved nonnull links reject capture/decoding.
Cross-document links and scene-set persistence remain unsupported.

Unknown/missing fields, wrong types, duplicate keys (including escaped spellings),
invalid values, nesting beyond 16 and component payloads over 64 KiB reject.
Registration is atomic; decode failures roll back staged objects. Transforms and
enablement use the scene/prefab envelope. Resolved environments and renderer
selection are not persisted. Qualification covers transforms, explicit selection,
checked lifetimes, configuration preservation, callback boundaries, codecs,
rollback and copied independent consumption. See
[local qualification](local-qualification.md) for platform and device limits.
