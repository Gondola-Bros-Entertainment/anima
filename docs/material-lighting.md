# Materials, sky and directional shadows

Anima supplies reusable material and environment rendering. A consuming game owns
its art, lighting values, world configuration, quality policy and performance targets.
The APIs accept engine resources and caller-supplied rendering settings.

## Material contract

The direct-light BRDF uses isotropic GGX, height-correlated Smith visibility and
Schlick Fresnel. Perceptual roughness is squared to obtain microfacet alpha and
floored at 0.045 for a finite lobe. Dielectrics use 0.04 normal-incidence reflectance;
metals derive it from base colour. Conventions follow the
[glTF metallic-roughness model](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#appendix-b-brdf-implementation).

Supported glTF inputs:

- Base colour and emissive textures decode from sRGB; factors and vertex colours
  are linear. Normal, metallic/roughness and occlusion textures remain linear data.
  A source image used as both colour and data gets separate encoded texture views.
- Metallic uses the blue channel; roughness uses green. Occlusion uses red and
  affects the ambient approximation only. Emissive is added before fog/exposure.
- Authored four-component tangents preserve handedness through skinning and
  mirrored instance transforms. Missing tangents use a fragment derivative frame;
  degenerate UVs fall back to the vertex normal.
- OPAQUE and MASK are supported. MASK tests texture alpha times material alpha
  times vertex alpha against the cutoff, identically in colour and shadow passes.
- KHR_materials_unlit is supported, including as a required extension. Unlit
  surfaces omit material lighting and shadow casting, but receive fog/exposure.
- All maps on a primitive share one UV set. That set may be TEXCOORD_1; conflicting
  sets and texture transforms are rejected. General BLEND remains unsupported.

Both sides render, retaining Anima's existing double-sided convention. Other
optional extensions are ignored; unsupported required extensions are rejected.
Ordinary mip RGB filtering follows the declared encoding and alpha is averaged
linearly. Mipmapped MASK base colours use alpha-weighted RGB to keep transparent
texels from contaminating leaf edges, followed by alpha coverage correction.
Roughness/normal variance filtering is not implemented.

### Cutout mipmaps

The pure `texture_mips(texture, TextureMipOptions)` overload accepts an optional
texture-space alpha cutoff in (0, 1]. It retains the original base level, creates
an uncorrected chain, then scales each lower level independently to approach the
base level's visible-texel fraction. The search accounts for RGBA8 rounding and
selects the nearest attainable coverage; ties favour the smallest change. It does
not propagate corrected alpha into subsequent downsampling or mutate the source.
The approach follows the coverage objective described in
[Computing Alpha Mipmaps](https://www.ludicon.com/castano/blog/articles/computing-alpha-mipmaps/).

`material_texture_plan` is a pure material-to-image preparation step. It derives
the effective texture cutoff as material cutoff / material alpha, deduplicates
equivalent uses and isolates different cutoffs from ordinary colour/emissive/data
uses of the same image. Unreferenced images are not uploaded. All-pass/all-reject
cutoffs and unmipmapped textures retain ordinary filtering. The mesh and shadow passes use these prepared material bindings. Work happens on scene
upload, with no additional shader operations or per-frame mip generation.

Coverage measures texel centres, not exact bilinear/trilinear screen-space area.
Very small mips and tied alpha values cannot represent arbitrary fractional
coverage (a 1×1 cutout is either present or absent). Varying vertex alpha still
multiplies the sampled value but is not included in the texture-only coverage
estimate. This improves mip consistency; it does not replace geometry LOD,
temporal antialiasing, alpha-to-coverage or target-camera visual qualification.

Programmatic materials default to metallic 0 / roughness 1. Imported glTF defaults
are metallic 1 / roughness 1. Existing per-instance colour overrides remain isolated.
Low-level RGB factors, metallic and roughness can change per draw. Texture bindings,
emission, mask parameters and other material-buffer constants require replacement.
Invalid values and structural mutation are checked before drawing.

## Environment API

`<anima/environment.hpp>` defines the headless configuration values. For authored
scenes, [directional-light and environment components](lighting.md) resolve
checked object links and transforms into the same `Environment` value, with
strict scene/prefab persistence. Direct configuration remains available below.

`VulkanRenderer::set_environment(Environment)` accepts linear RGB lighting and
finite settings between draws. Defaults are
two directional lights, constant ambient, no sky/fog/tone mapping/shadows. Invalid
settings leave the accepted environment intact. `set_view` takes a finite invertible
Vulkan view-projection matrix and derives the camera origin and inverse matrix.

The environment supplies sun and fill directions/radiance, hemispherical ambient,
ambient specular approximation, sky colours, distance fog, exposure and optional
Reinhard tone mapping. The art-directed sky gradient and sun disc use the same sun
direction as surfaces. This is a default gradient background, not physical
atmospheric scattering or image-based lighting. The renderer draws into one
linear RGBA16F world target before final display conversion. Reflection convolution, global illumination,
bloom and subsurface shading remain unimplemented.

The math is deliberately small: diffuse illumination depends on max(N dot L, 0);
fog transmission is exp(-density * distance); exposure scales linear light; optional
display compression is colour / (1 + colour), followed by sRGB encoding exactly once.
UI keeps its existing colour path and does not receive world fog or tone mapping.

## Directional shadow regions

`Environment::shadow` specifies an optional centre, half-extent, depth, resolution
and receiver biases. A 2,048 map across 40 metres has a nominal 1.95 cm texel pitch.
The region is orthographic in light space and snapped to texels to reduce movement
shimmer. Resolution is checked against physical device image/framebuffer limits.
There is no automatic quality scheduler or claim of unlimited shadow coverage.

`Environment::detail_shadow` adds an optional independently positioned region
with the same settings contract. Use it to give a nearby subject finer shadow
texels while retaining the world region. The application follows the subject by
updating its center through `set_environment`; both projections snap to their own
texel grids. The detail sample blends into the world sample across the outer four
percent of its light-space volume. It does not multiply two shadow factors or fade
world shadows to white. Its caster list is independently culled, including casters
outside the original world region or main camera.

The depth pass includes GPU-skinned and rigid geometry and honours explicit
visibility and alpha masks. Its frustum is independent of the main camera: a
caster outside the camera can still shade a visible receiver. Shadow-only instances
still need palettes. Main-view culling counters continue to describe the main view;
shadow draws/indices and allocation bytes have separate counters.

The receiver bilinearly interpolates a 3x3 comparison kernel using its 4x4 union
of texels (16 fetches). Each axis has weights `(1-f, 1, 1, f)` for the fractional
texel position `f`; the two-dimensional weights sum to nine. This filters shadow
coverage instead of snapping the kernel between texels or interpolating raw depth.
See the comparison-filtering principle in
[GPU Gems: Shadow Map Antialiasing](https://developer.nvidia.com/gpugems/gpugems/part-ii-lighting-and-shadows/chapter-11-shadow-map-antialiasing).
Each comparison uses the actual receiver plane's depth at that texel's centre, plus the
configured residual constant/slope bias. Screen derivatives of world position
are transformed by the orthographic light matrix; their cross product yields
the plane and its depth gradient in shadow UV coordinates. Derivatives are taken
before alpha discard. Shading normals and normal maps do not define this plane.
Nearly edge-on, ill-conditioned projections fall back to the residual bias.
This removes the incorrect self-shadowing caused by comparing every neighbour
against one centre depth. It does not remove all silhouette, curvature or
resolution limitations. The explicit region fades at its boundary.

For background on why filtered comparisons need slope-aware depth treatment,
see NVIDIA's [discussion of PCF bias](https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-8-summed-area-variance-shadow-maps).
The renderer supports one world region and one optional detail region. The
application positions them explicitly; there are no automatic frustum-split
cascades. Choose coverage that includes relevant casters.

Depth targets and descriptors retire behind the existing frame fence. A disabled
shadow uses a cleared 1x1 fallback image. With profiling enabled, GPU timestamps
separate shadow, main scene/sky, display conversion/UI, capture/transitions and total
work. They describe the previous completed submission and exclude presentation;
unavailable timestamps are not zero cost. Shadow counters and GPU timing include
both regions. Each enabled region adds a depth target and a geometry pass; a
disabled detail region has a cleared 1x1 descriptor placeholder. Profiling is opt-in.

## Verification

CPU and external-consumer tests exercise numeric validation, inverse projection,
shadow texel stability, colour/data mip encodings, tangent/alpha seams, material
immutability and source import. The external environment consumer covers masked
shadow holes, an off-camera caster, CPU/GPU scene parity, hidden casters, resize,
invalid-setting rollback, empty sky and clean synchronization validation.

```sh
cmake --build --preset desktop
ctest --preset desktop
python3 tools/engine/gpu_material_smoke.py build/desktop/anima
python3 tools/engine/gpu_surface_smoke.py build/desktop/anima
python3 tools/engine/gpu_environment_smoke.py build/desktop/consumer-desktop/build/consumer
python3 tools/engine/gpu_foliage_smoke.py build/desktop/consumer-desktop/build/consumer
python3 tools/engine/gpu_resource_smoke.py build/desktop/consumer-desktop/build/consumer --asset MODEL.glb
```

The foliage runner checks known visible areas before/after minification, isolates
two mask cutoffs from ordinary RGB/emissive use of the same image, and compares
CPU-deformed static references with GPU-skinned scenes. The environment runner also moves the camera across
the fixed shadow region and checks exact restoration of the original image.

The surface-map runner compares normal maps to equivalent geometric normals,
linear G/B maps to scalar factors, sRGB emission to decoded factors and a shared
colour/data source texture to equivalent constants. It also checks derivative
frames and required unlit extension handling. GPU commands require a normal desktop
Vulkan environment. Save local evidence under build/ and qualify each consuming
application's authored assets, camera motion, dense foliage and target hardware.
