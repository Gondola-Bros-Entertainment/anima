# Rendering visibility

Anima owns generic visibility mechanisms. A consuming game supplies immutable
render assets, instance transforms/poses, explicit mesh visibility and cameras.
No character IDs, gameplay rules, transport relevance or fixed population/vertex
ceiling enter the culling API.

The directional shadow pass owns a separate frustum and draw list. A main-view
rejection does not reject a shadow caster; palette preparation covers instances
needed by either view. See [materials and lighting](material-lighting.md) for the
explicit shadow-region contract and its current coverage limitations.

## Frustum tests

`Scene::Instance::bounds` is the union of its animated primitive bounds.
Pose publication updates the palette, primitive bounds and union together after
validation. The union includes hidden equipment so a visibility edit cannot
invalidate it. It can be looser than the visible geometry; that costs extra work
without clipping a character or weapon.

`anima::RenderFrustum` is a graphics-independent public type supplied by the
`anima::assets` library target:

```cpp
#include <anima/assets/render_visibility.hpp>

anima::RenderFrustum frustum(view_projection);
const bool potentially_visible = frustum.intersects(scene.instance(id).bounds);
```

It extracts the six homogeneous Vulkan clip inequalities from a column-major
view-projection matrix: `-w <= x,y <= w`, `0 <= z <= w`. The depth convention
matches Anima's current pipeline, which enables depth clipping. See the
[Vulkan primitive clipping specification](https://docs.vulkan.org/spec/latest/chapters/vertexpostproc.html#vertexpostproc-clipping).
The test supports perspective and orthographic matrices, including reversed-depth
and infinite-far projections. Those are capabilities of this visibility test;
the renderer's depth-clear/compare policy remains a separate pipeline concern.

Plane tests use the support corner of each AABB, double intermediates and a
scale-relative float rounding margin. They do not divide by clip `w`, normalize
degenerate planes, or require a box corner to lie inside the frustum. Unknown or
invalid bounds are conservatively retained; nonfinite camera matrices are rejected.
An unset frustum retains all bounds. Bounds touching a plane or enclosing the eye
remain eligible. This is conservative rejection, not an exact polyhedron overlap
test: some invisible geometry can be retained.

The rounding margin uses the absolute original camera rows before plane extraction.
Nearly cancelling rows can produce a small plane while the separate float clip
coordinates still have a larger rounding error; deriving the margin from that
small plane alone could discard a GPU clip-boundary contact.

The Vulkan resource renderer enables culling by default. Before preparing a
frame, it tests the instance union, then the explicitly visible mesh parts of
intersecting instances. Only surviving indexed draws enter the draw list. A fully
culled instance contributes no palette upload or draw commands. Partial instances
currently upload their complete palette. Source poses, explicit visibility,
membership and assets remain unchanged. A camera or pose change is reflected on
the next draw, without visibility history or a delayed GPU readback.

```cpp
renderer.set_view(view_projection);
renderer.set_scenes({scene});
renderer.set_frustum_culling(false); // Unculled reference, between draws.
renderer.set_frustum_culling(true);  // Default for resource scenes.
```

Explicit scene selection preflights visible resources independently of the previous
camera. Subsequent offscreen additions may wait until first visibility for their
GPU upload. Culling does not evict cached assets: existing external asset ownership
and frame-fenced retirement govern residency. A previously uploaded returning
instance reuses geometry and uploads its current pose. Tests can disable culling
for an unculled reference.

`ResourceStats` reports current prepared candidates, culled instances/draws,
submitted instances/indices, actual draw calls and palette upload bytes. Candidates
exclude explicit hidden instances/parts and empty draws. Counters become useful
after a successful resource frame; they are neither total GPU memory nor FPS.
No frame is dropped because an arbitrary culling-count threshold was exceeded.

## Scaling

The CPU visits active instances and visibility flags each frame, and consumers
still evaluate poses for offscreen objects. Candidate/draw/palette scratch can
allocate during preparation. Culling cannot reduce an all-visible gathering.
Network relevance remains an independent consumer decision.

## Verification

`render_visibility` covers all six clip planes, contact, enclosing/spanning boxes,
behind-eye cases, perspective/orthographic/reversed/infinite projections, large
coordinates, invalid bounds and 20,000 boxes checked against an independent
homogeneous-corner oracle. Existing indexed-reference tests verify that the
instance union contains every primitive bound across independently posed skins.

The external consumer's `--culling` mode uses only public APIs. Run
`tools/engine/gpu_culling_smoke.py CONSUMER [--asset MODEL.glb]` to compare exact
pixels with culling off/on across animated poses, camera edges, near/far planes,
resize, explicit visibility and an empty view. It verifies zero pose/draw work
for a fully culled view, shared geometry residency, return-to-view behavior and
clean Vulkan validation. It supplements the existing CPU/GPU skinning parity,
resource rollback and retirement checks.

Large-zone qualification additionally needs measured camera motion, churn, dense
all-visible combat, materials/foliage overdraw and memory pressure on target devices.
Small correctness fixtures and a successful current game session do not replace
those workloads.
