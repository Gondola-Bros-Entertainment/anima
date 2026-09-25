# Scene cameras and view selection

`<anima/camera.hpp>` supplies `Camera`, `CameraView` and `view_matrix` through
`anima::assets`. They use the production Scene/component storage and require no
SDL, Vulkan, physics backend or display. Applications own camera movement,
switching policy and the drawable aspect ratio.

```cpp
anima::Scene scene;
auto eye = scene.create("Camera");
eye.set_position({0, 2, 5});
auto lens = eye.add_component<anima::Camera>();
auto view = scene.create("View").add_component<anima::CameraView>();
view->camera = eye;

// After component updates; use the drawable's current width / height.
renderer.set_view(anima::view_matrix(scene, aspect));
```

A camera looks along local **-Z**, with **+Y** up and **+X** right. Its position
and orientation come from its object's world transform, including parents. World
axes must have lengths of at least 0.0001, be orthogonal within 0.0001 after
normalization, and form a right-handed basis. Positive nonuniform scale is ignored;
shear, singular axes and reflected bases reject. A scaled parent with a rotated
child can produce shear and therefore reject. These restrictions apply when
resolving a view; general scene transforms retain their existing affine contract.
Audio listeners use local +Z forward; an application pairing a listener with this
camera must author the listener's orientation accordingly.

`CameraSettings` defaults to a 45-degree vertical perspective lens, near plane
0.1 and far plane 1000. `projection` selects perspective or orthographic.
`orthographic_height` is the full vertical extent in world units, defaulting to
10. The horizontal extent follows the supplied aspect ratio. Both lens fields
are stored and validated, even when inactive, so switching projection preserves
their authored values. Vertical field of view is 1–179 degrees inclusive;
orthographic height and clipping distances are bounded by 0.0001 and one billion,
with far strictly greater than near. Every number must be finite. `configure`
validates the complete settings before replacing accepted state.

`view_matrix(Scene|SceneSet, aspect)` requires exactly one active `CameraView`
across the supplied selection. Other views can remain disabled or inherit inactive
object state. The selected object must be live, belong to that Scene or SceneSet,
and have an active Camera component. Other cameras need not be disabled. Multiple
active views reject even when they point to the same camera. No active view, a
null/stale/foreign link, an inactive or missing camera, an invalid transform, or a
nonpositive/nonfinite aspect rejects. Finite aspects that cannot produce a finite,
invertible float matrix also reject. Applications should avoid extreme depth
ratios and world coordinates when they need useful float depth precision.

The view link identifies the object's current Camera attachment. Removing that
attachment makes resolution fail; adding a new Camera on the same live object
allows resolution again. Destroying the object or unloading/replacing its scene
invalidates the link permanently. No name/key lookup silently repairs it.
`SceneSet::active` never chooses the view. A view can point across scenes in a
runtime set, but resolving that view's standalone scene rejects a foreign camera.

Resolution is a read-only operation on idle scenes after updates, with the same
callback/construction/SceneSet mutation restrictions as other scene drivers. It
does not run component hooks, advance time, retain a scene, or mutate a renderer.
The returned matrix uses Vulkan Y-down and zero-to-one depth and works with the
existing renderer, culling, shading and shadow paths. Call `set_view(view_matrix(...))`
before drawing each changed view. If resolution throws, no renderer call occurs,
so its previously accepted view remains. Applications decide whether to repair
selection, skip drawing or present that previous view.

Renderer scene selection remains explicit and independent: `set_scenes` controls
which geometry is drawn, while the Scene/SceneSet supplied to `view_matrix` controls
where the view and camera are resolved. Refresh both deliberately after a transition.
There is one view per renderer; this API does not provide split-screen, viewport
rectangles, camera stacking, render textures, automatic tracking or editor controls.

## Persistence

```cpp
anima::ComponentCodecs codecs;
anima::add_camera_component_codecs(codecs);
auto document = anima::serialize_scene(scene, {}, codecs); // No meshes in this example.
auto restored = anima::load_scene(document, {}, codecs);
renderer.set_view(anima::view_matrix(*restored, aspect));
```

Strict `anima.camera.v1` payloads contain exactly `projection` (the string
`perspective` or `orthographic`), `vertical_fov_degrees`, `orthographic_height`,
`near_plane` and `far_plane`. `anima.camera-view.v1` contains exactly `camera`,
a canonical object-key string resolved through `ObjectReferences`. Null is `"0"`:
it can be saved as unconfigured state but cannot produce a view. Enablement,
activation and transforms use the existing scene/prefab envelope. Aspect ratio,
renderer selection and previously resolved matrices are not persisted.

The codecs use the current strict scene/prefab **v3** format. Forward/backward and
cross-root camera links inside a scene resolve against the complete loaded graph;
each prefab copy remaps links to its own objects. Missing Camera attachments or
inactive selections remain authored state and fail at view resolution, not loading.
Stale, out-of-prefab, foreign and unresolved nonnull object links reject capture or
decoding. Cross-document links and scene-set persistence remain unsupported.

Unknown/missing fields, wrong types, unknown projection strings, duplicate keys
(including escaped spellings), invalid values, nesting beyond 16 and payloads over
64 KiB reject. Codec registration is atomic. Failed loading/instantiation uses the
existing staged-object rollback and leaves published scenes intact.

Qualification covers analytic clip coordinates and culling, hierarchy/scale,
selection and activation, callback boundaries, checked lifetimes, strict payloads,
rollback and independent prefab/scene consumption. The copied Vulkan consumer
compares perspective/orthographic and moved views, restored scene-camera pixels,
and unchanged pixels after a rejected stale selection. See
[local qualification](local-qualification.md) for platform and physical-device limits.
