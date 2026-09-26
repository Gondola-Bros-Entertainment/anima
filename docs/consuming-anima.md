# Consuming Anima from another C++ project

Pin an Anima source commit in the consuming repository and use its public CMake
targets. No installed SDK/package is provided yet. The small
[consumer fixture](../tests/consumer/CMakeLists.txt) is a standalone application;
CTest copies only its own CMake/C++ files outside the engine source directory,
then builds Anima as a source dependency.

## Build options and runtime

| Option | Effect |
| --- | --- |
| `ANIMA_BUILD_ASSETS` | Builds `anima::assets`, including headless assets, animation and scene instances |
| `ANIMA_BUILD_AUDIO_OUTPUT` | Opt-in `anima::audio_output` SDL device adapter without assets or Vulkan; defaults off |
| `ANIMA_BUILD_DESKTOP` | Builds `anima::desktop` with SDL3/Vulkan; assets are optional for the existing triangle mode |
| `ANIMA_BUILD_DOCS` | Optional local Doxygen reference target; defaults off, not needed by consumers |
| `ANIMA_BUILD_INPUT_SDL` | Opt-in SDL event converter using headers only; defaults off |
| `ANIMA_BUILD_PHYSICS` | Opt-in headless Jolt world and, with assets, scene/prefab targets; defaults off |
| `ANIMA_BUILD_PHYSICS2D` | Opt-in headless Box2D world and scene/prefab targets; requires C17 and C++20; defaults off |
| `ANIMA_BUILD_UI_DOCUMENTS` | Opt-in RmlUi document target without graphics; defaults off |
| `ANIMA_BUILD_UI` | Opt-in `anima::ui` RmlUi integration; requires desktop, assets optional; defaults off |
| `ANIMA_BUILD_TOOLS` | Builds `anima_headless`, `anima_inspect` when assets are enabled, and the `anima` viewer when desktop is enabled |
| `ANIMA_BUILD_TESTS` | Builds/registers engine tests independently of tools; external consumer checks are included |

`anima::core` is always available. Tool/test defaults are off when Anima is a
subdirectory. At the top level, tools default on and tests initially follow
`BUILD_TESTING`; the explicit cached `ANIMA_BUILD_TESTS` option controls subsequent
configuration. The existing core/headless/desktop presets keep their tools and
tests; desktop explicitly enables audio output as well. Anima does not change a
parent project's `BUILD_TESTING` choice.

For a desktop consumer:

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyGame LANGUAGES CXX)

set(ANIMA_BUILD_TOOLS OFF)
set(ANIMA_BUILD_TESTS OFF)
set(ANIMA_BUILD_ASSETS ON)
set(ANIMA_BUILD_DESKTOP ON)
add_subdirectory(external/anima)

add_executable(my_game main.cpp)
target_link_libraries(my_game PRIVATE anima::core anima::assets anima::desktop)
anima_copy_sdl_runtime(my_game)
```

The runtime helper serves both `anima::desktop` and `anima::audio_output` and
copies the selected shared `SDL3::SDL3` library beside the
consumer executable on Windows. It is idempotent, validates the executable target,
and is a no-op for static SDL or other platforms. Call it from the executable's
directory after linking. Imported SDL targets are made globally visible so a parent
consumer can use them; this uses CMake's supported
[`find_package(... GLOBAL)` behavior](https://cmake.org/cmake/help/v3.25/command/find_package.html).
The helper is not an installer and does not package Vulkan drivers or game assets.
Windows DLL execution still needs qualification on Windows.

For headless gameplay/asset tests, set `ANIMA_BUILD_DESKTOP OFF` and omit
`anima::desktop`. For a core-only executable, also turn the asset option off.
With audio output, SDL input and UI left off, neither configuration searches for
or links SDL/Vulkan/RmlUi/FreeType. Compiler warning policy
is private to Anima targets; the consumer sets its own policy.

## Shared render resources and visibility

For rendered scenes, use `Mesh` and `Scene` from
`anima::assets`. Compile an immutable asset once and share it across independently
posed instances. The Vulkan renderer caches indexed GPU geometry by asset identity
and performs skinning in its vertex shader. Poses and animation clocks remain
owned by the consumer.

```cpp
#include <anima/scene.hpp>
#include <anima/desktop/vulkan_renderer.hpp>

auto compiled = anima::Mesh::load(asset_path);
auto scene = std::make_shared<anima::Scene>();
auto actor = scene->create("Actor", compiled);
actor.transform().set_position({0, 0, 0});

anima::VulkanRenderer renderer(window, {});
renderer.set_view(view_projection);
renderer.set_scenes({scene});
// Update poses, membership, material factors and explicit visibility between draws.
renderer.draw();
```

The renderer automatically rejects off-camera instances and mesh parts using
conservative animated bounds. A changed camera or pose takes effect at the next
draw. It does not change source visibility flags, animation time or networking
relevance. Disable it with `renderer.set_frustum_culling(false)` for an unculled
reference. Counters expose candidates, culled/submitted work and palette bytes.

Headless consumers can query `RenderFrustum::intersects` directly without SDL or
Vulkan, as the external asset consumer demonstrates. See the
[visibility contract](../include/anima/assets/render_visibility.hpp) for the public query, clip convention,
conservative behavior, resource lifetime and scaling characteristics.
Mutations are single-threaded between draws. Render-scene handles are scene-specific
and generational; removing an instance invalidates its old handle.

The [scene and object guide](scene-objects.md) covers GameObject lifetime, its always-present
transform, optional mesh presentation, terrain and simple animation.

## Instance updates and diagnostic snapshots

`Scene::Id` includes its scene owner, slot and generation. Removal invalidates
that handle; slot reuse cannot make a stale handle valid. Share one `Mesh`
across instances to share immutable geometry and textures.

`set_pose(id, pose, world)` accepts the sampled pose and instance transform together.
Use `sample_pose` or `pose_from_local` to compute consistent world matrices; changing
`Pose.local` alone does not recalculate `Pose.world`. `Animator` offers explicit clip playback; advanced evaluators and game authority
remain caller-controlled.

Material and primitive indices are asset-local. A material factor override replaces
the authored linear RGB factor, including an originally zero factor, and multiplies
vertex colour and the texture. Clearing it restores the authored factor. Whole-instance
visibility combines with per-primitive choices; hiding an instance preserves those
choices. Animated bounds are conservative and may include hidden parts.

`Mesh::compile` validates and copies source data into immutable indexed
resources. Scene setters validate changing instance data before publishing it.
Invalid updates preserve the accepted instance. Scene mutation and rendering run
on one thread, between draws; a const shared pointer does not synchronize access.

For explicit inspection, `Scene::snapshot(budget)` returns an owning CPU
`MeshSnapshot`. `make_mesh_snapshot` and `pose_mesh_snapshot` independently deform source geometry for
reference calculations. These functions are outside the rendering loop. Snapshot
budgets and their limits are described in the [resource guide](../include/anima/assets/scene_budget.hpp).

## Renderer scene replacement

`VulkanRenderer::set_scenes` selects a list of scenes and an empty list clears it.
`RendererOptions::scenes` accepts the same list during construction. Null or
duplicate entries reject. Use `{scene}` for one scene or `set.render_scenes()` for
an explicit snapshot of a SceneSet's current membership. Refresh selection after
replacement/loading; retained views of unloaded scenes become empty.

```cpp
auto next = std::make_shared<anima::Scene>();
auto object = next->create("Object", compiled);
renderer.set_scenes({next});
object.transform().set_matrix(world);
renderer.set_scenes({});
```

Call selection on the rendering thread between draws. It waits for prior graphics
use, prepares independently owned resources, then accepts the list. Recoverable
preparation failures preserve the selected scenes. Replacement keeps the window,
surface, swapchain and presentation synchronization. It is valid while minimized;
`draw` returns false while the drawable is unavailable. The renderer retains the
scene pointers and borrows the SDL window, which must outlive it.

Membership, poses, factors and visibility can change through `Scene` setters.
To change mesh or texture content, compile a new asset. Already selected scene
mutations are caller-owned: if incremental preparation raises `SceneResourceError`,
repair or clear that scene before continuing. An allocation failure during new
scene selection instead leaves the previously selected scene usable.

`RendererFatalError` means drawing cannot continue. Shut down after device loss,
fatal Vulkan draw failures or a graphics/upload fence timeout. Ordinary acquire
not-ready/timeouts return false. Shutdown is idempotent; `RenderStats` includes
scene generations (initial state counts as one), captures and validation through
cleanup. Upload work retires before its staging and candidate resources are freed.
A hung driver during retirement still requires an external process watchdog.

GPU preparation is synchronous and cached by compiled asset identity. Its API and
limits are in [resource preparation](../include/anima/desktop/vulkan_renderer.hpp). Failure injection
is explicitly for tests. Ordinary empty scenes draw the background; the viewer's
diagnostic triangle also exercises desktop builds without asset support.

Enable `ANIMA_BUILD_UI` and link `anima::ui` for HUDs and menus. The
[UI guide](ui-integration.md) covers RmlUi handles, fonts, input, DPI and the draw
loop. Game screens and navigation belong to the consuming project.

## Independent actor presentation

For separate model/motion resources, phased actions, semantic attachments and
coordinated actors, use the public [presentation APIs](animation-evaluation.md).
The asset consumer builds two unrelated skeleton layouts and all its documents
from scratch. The core consumer also exercises capability resolution without
importing assets or graphics. The consuming game supplies content, clocks and
authority; it does not need copies of the engine's motion/action loaders.

See [3D physics](physics.md) and [2D physics](physics2d.md) for optional module contracts and build requirements.

## Action input

`anima::core` provides headless action maps and contexts; `anima::assets` adds
scene/prefab configuration. Enable `ANIMA_BUILD_INPUT_SDL` and supply
`ANIMA_INPUT_SDL_INCLUDE_DIR` for optional `anima::input_sdl`, a pure event
converter needing SDL headers but no SDL runtime or graphics. Applications own
their SDL event pump and decide game responses. See [action input](input.md).

## Audio components

`anima::core` owns clips and offline mixing. `anima::assets` supplies listener/source
components, explicit transform/enablement synchronization, and versioned
configuration codecs with caller clip resolvers. For SDL device output, enable
`ANIMA_BUILD_AUDIO_OUTPUT` and link `anima::audio_output`; its public header is
`<anima/audio_output.hpp>`. Desktop and audio output remain independently selected.

An audio-only consumer needs no scene or graphics dependency:

```cmake
set(ANIMA_BUILD_TOOLS OFF)
set(ANIMA_BUILD_TESTS OFF)
set(ANIMA_BUILD_ASSETS OFF)
set(ANIMA_BUILD_DESKTOP OFF)
set(ANIMA_BUILD_AUDIO_OUTPUT ON)
add_subdirectory(external/anima)
add_executable(my_audio main.cpp)
target_link_libraries(my_audio PRIVATE anima::audio_output)
anima_copy_sdl_runtime(my_audio)
```

SDL runtime discovery is shared by audio output and desktop; use an installed
SDL3 package or `ANIMA_FETCH_SDL` for the pinned source. Input-only conversion
continues to use SDL headers without configuring a runtime. See the
[audio contract](audio.md) for output ownership and dummy-device qualification.
