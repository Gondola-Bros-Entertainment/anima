# Rendering resources and ownership

Anima renders static and animated meshes through shared indexed `Mesh`
resources and `Scene` instances. Games own content, animation clocks,
streaming priorities and networking. The renderer owns GPU allocation, drawing,
synchronization and resource retirement.

## Resource ownership

| Object | Responsibility |
| --- | --- |
| `Asset` | Imported hierarchy, source geometry, materials, skins and animation channels |
| `Mesh` | Immutable compiled geometry, indexed draws, materials, skin bindings and influence bounds |
| `Scene` | Instance membership, independent palettes, material factors and visibility |
| `VulkanRenderer` | GPU resource cache, palette uploads, visibility, rendering passes and retirement |
| CPU `MeshSnapshot` | Owning diagnostic geometry and material data for inspection and reference calculations |

Compile once and share the resulting `Mesh`. Each compiled asset owns one
cached GPU vertex/index allocation and its material/texture resources. Distinct
compiled assets currently own separate images, even when their pixels match.
Material samplers are shared by filtering, wrapping and mip LOD policy. Weak cache
entries do not extend a sampler's lifetime beyond its last material owner.

## Geometry and animation

Indexing compares every source vertex attribute within a primitive: position,
normal, colour, UV, skin indices/weights, tangent and alpha. It preserves seams and
triangle order. It does not simplify geometry. Instances have independent poses,
material factors and visibility while sharing the immutable resource.

Vertex-shader skinning uses blended matrices for position and inverse-transpose
normal transforms. The independent CPU `pose_mesh_snapshot` calculation verifies these
results for nonsingular transforms. Degenerate blends produce a finite up normal
in the shader; CPU normal transformation rejects singular matrices. This difference
is not an exception-equivalence guarantee for degenerate poses. Animated bounds
conservatively combine influence boxes with padding for accepted weights and
floating-point roundoff.

The default pipeline owns one RGBA16F scene-colour target and one depth attachment.
It converts scene colour for display, then draws UI. No effect-specific composition
target, mesh buffer or shader is allocated. `world_target_bytes` counts that colour
target and depth allocation, excluding swapchain and shadow images.

GPU geometry is device-local and uploaded through transfer staging. One graphics
frame is in flight. Its fence retires readers before palette updates, descriptor
replacement or unused resource destruction. Submitted uploads retire before their
staging and candidate objects unwind, including exception paths. Whole-allocation
mapped flushes satisfy noncoherent alignment. Push constants occupy 128 bytes and
palette matrices 64 bytes, with corresponding C++/shader layout assertions.

Device checks cover interface limits, image dimensions, indexed-draw addressability
and storage-buffer ranges. Full 32-bit indexing is enabled only when supported.
Palette storage grows geometrically up to the reported storage-buffer range;
chunking beyond it and pooled geometry allocation are not implemented.

## Scene selection and residency

`set_scenes` accepts an explicit list of resource scenes; an empty list clears
selection. Null or duplicate entries reject before preparation. Objects from all
selected scenes share the same mesh cache, palette buffer, visibility and shadow
passes. Failed selection preserves the previous list. Mutations to an already selected scene
remain the caller's responsibility: incremental preparation can raise
`SceneResourceError`, after which the caller repairs or clears its scene.
`RendererFatalError` requires shutdown. See the
[consumer contract](consuming-anima.md#renderer-scene-replacement).

Assets with external owners retain their cached GPU resources. Unowned entries
retire after the frame fence. Visibility affects submitted draws and palettes;
it does not evict the source asset. Independent preparation uses the same cache
and lifetime rules; see [resource preparation](resource-preparation.md).

`resource_stats()` reports mesh uploads, geometry bytes, current geometry/texture
allocations, pose storage/uploads, draws, instances, cached assets and samplers.
These counters exclude some staging, UI, swapchain and driver allocations; they
are not total process/device memory. Use the [profiling section](#profiling) to
interpret CPU/GPU timing scopes.

## Diagnostic snapshot budget

`Scene::snapshot` explicitly expands CPU geometry. Rendering never calls it.
`SceneGeometryBudget::vertex_bytes` limits this one vertex payload, defaulting to
256 MiB. `SceneCapacityError` reports requested and allowed bytes before allocation.
A rejected snapshot leaves the render scene intact. `validate_scene` applies the
same policy when validating a CPU snapshot.

This budget excludes source assets, textures, metadata, staging and GPU residency.
It does not limit the resource renderer. Index/address arithmetic guards remain
active independently of the diagnostic snapshot policy.

## Profiling

`RendererOptions::profile` enables CPU stage timings and five GPU timestamp queries
when the graphics queue supports them. Read `VulkanRenderer::frame_profile()` after
a successful draw. Profiling defaults off, with no query pool or timing calls.

CPU timings cover the graphics-fence wait, resource preparation/upload, acquisition,
record/submission and presentation. `uploaded_bytes` counts pose palettes for the
rendered scene; it excludes UI geometry and push constants. These are elapsed
scopes, not CPU utilization. Driver calls may block inside record/submission.

GPU timings describe the previous submitted command buffer, read after its existing
graphics fence. They cover shadow rendering, opaque scene/sky,
display conversion/UI, and capture/transitions, plus the full command-buffer
interval. `gpu_available=false` means unavailable, not zero cost. GPU timings do
not measure display latency. CPU and GPU overlap; do not add them to estimate FPS.

`resource_stats()` reports geometry/texture residency, geometry and palette uploads,
draws, visibility candidates and shared material samplers. Counter boundaries are described above.

Measure optimized application builds with real assets and stable drawable pixel
dimensions. Warm up after scene replacement before associating samples with a new
scene. Keep capture/validation and performance runs separate, and avoid concurrent
builds or game clients. Record source revisions, asset hashes, camera, warmup and
sample counts alongside results under `build/`.

Run `ctest --preset headless` and `ctest --preset desktop` for correctness. Use the
[resource GPU checks](render-resource-architecture.md) for independent deformation,
resource lifetime and failure tests. Sustained game/crowd performance is qualified
by the consuming application.

## Verification

CTest exercises immutable resources, indexed seams, independent pose/material
state, conservative bounds, stale handles, snapshot budgets and an external
application consuming only public targets and headers.

```sh
python3 tools/engine/gpu_resource_smoke.py build/desktop/consumer-suite/build/consumer_desktop --fatal
python3 tools/engine/gpu_replacement_smoke.py build/desktop/consumer-suite/build/consumer_desktop --viewer build/desktop/anima
```

The resource runner compares CPU-deformed static geometry against GPU skinning,
using the same indexed renderer. It covers poses, materials, visibility, optional
GLB clips, join/leave restoration, last-owner retirement and injected allocation/
upload failures. Image tolerance is mean channel error below 0.5 on a 0–255 scale
and fewer than 0.2% of pixels differing by more than 16 in a channel. Rollback
images match exactly. The sampler regression exercises 600 distinct assets and
seven distinct sampling policies. Replacement checks preserve the window and
swapchain across scene changes, including minimized replacement and shutdown.

Write run logs and captures under `build/`. GPU checks qualify
the actual host/backend; injected failures do not establish real device-loss
recovery. Sustained gameplay/crowd qualification belongs to the consuming game.
