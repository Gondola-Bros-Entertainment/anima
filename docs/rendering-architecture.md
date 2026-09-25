# Rendering architecture

`VulkanRenderer` owns a fixed default pipeline: mesh resource preparation,
directional shadows, opaque/cutout meshes, a gradient background, display
conversion and UI composition. Public configuration covers built-in materials,
view transforms and environment settings. It accepts no custom shader programs,
material binding layouts or render passes.

| Layer | Responsibility |
| --- | --- |
| Graphics backend | Device capabilities, buffers/images, descriptors, pipelines, submission, synchronization and retirement |
| Default pipeline | Visibility, mesh/skinning draws, material shading, shadows and display conversion |
| Consuming application | Content, authored appearance, view/environment settings, quality policy and gameplay |

An explicit list of selected `Scene`s supplies live objects to one view. All
scenes share mesh residency, culling, pose storage and shadow passes. Selection
and mutation occur on the rendering thread between draws. Failed resource
preparation during selection preserves the previous list; fatal device errors
require renderer shutdown. An empty list clears the background; null or duplicate
entries reject. Selection never follows `SceneSet::active` implicitly.
See [resource ownership](render-resource-architecture.md) for allocation,
synchronization, failure and diagnostic contracts.

The background is a gradient with a directional-light disc. The renderer has no
cubemap skybox or water/cloud module. Optional effects must own their resources
and passes explicitly without imposing allocations or world assumptions on
unrelated applications. Engine/application responsibilities are defined in
[engine boundaries](engine-boundaries.md).

See [materials and lighting](material-lighting.md) for shading and environment
configuration, [visibility](render-visibility.md) for culling, and
[resource preparation](resource-preparation.md) for synchronous uploads.

[Scene cameras](cameras.md) supply perspective/orthographic lenses and explicit
view selection through the production component APIs. Resolve `view_matrix` after
scene updates and pass it to `set_view`; drawable aspect ratio and camera movement
remain application inputs. Camera/view components persist in strict scene/prefab
v3 documents, independently of the renderer's explicit geometry selection.

[Scene lighting](lighting.md) resolves transform-driven directional lights and
explicit environment links into the existing `set_environment` value. The
headless component API validates a complete Scene/SceneSet selection before
publication; the renderer still supports its two directional slots and existing
shadow regions. Camera, geometry and lighting selection remain independent.
