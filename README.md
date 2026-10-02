# Anima

Anima is a modular C++20 game engine. It provides a headless runtime for scenes,
animation, physics, audio, input and navigation, with an optional SDL3/Vulkan
renderer and RmlUi interface layer. Applications compose the libraries they need
through public CMake targets.

The engine is under active development. It does not yet provide an editor, project
manager or standalone player.

## Build

The headless runtime requires CMake 3.25+, Ninja and a C++20 compiler. Each preset's
workflow configures, builds and tests it:

```sh
git clone https://github.com/Gondola-Bros-Entertainment/anima.git
cd anima
cmake --workflow --preset headless
```

The `core` preset builds only the dependency-free library. The `desktop` preset adds
the Vulkan renderer and the `anima` viewer:

```sh
cmake --workflow --preset desktop
./build/desktop/anima --validation
```

Desktop builds need SDL3 3.2+ (installed, or `-DANIMA_FETCH_SDL=ON` for the pinned
SDL 3.4.16) and a Vulkan SDK with `glslc` whose headers declare
`VK_EXT_surface_maintenance1` and `VK_EXT_swapchain_maintenance1`. At runtime the
renderer needs Vulkan 1.1, through MoltenVK on macOS. `--validation` requires the
Vulkan validation layer, and the UI modules require FreeType. `anima --help` lists
the viewer's options; with several Vulkan drivers installed, choose one with
`VK_DRIVER_FILES`. Tests use synthetic fixtures and never open a window. See
[contributing](CONTRIBUTING.md) for the full configuration, sanitizers and GPU checks.

## Use Anima in a project

Add a pinned checkout as a subdirectory and link the targets you need:

```cmake
add_subdirectory(external/anima)

add_executable(my_game main.cpp)
target_link_libraries(my_game PRIVATE anima::assets anima::desktop)
anima_copy_sdl_runtime(my_game)
```

`anima_copy_sdl_runtime` copies a shared SDL3 library beside the executable on
Windows and does nothing elsewhere. `ANIMA_BUILD_DESKTOP` and `ANIMA_BUILD_ASSETS`
default to on; switch them off for headless or core-only consumers, which then
configure no SDL, Vulkan or RmlUi. Tools and tests default to off when Anima is a
subdirectory, and compiler warning policy stays private to Anima's targets.

| Module | Targets | Option | Provides |
| --- | --- | --- | --- |
| Core | `anima::core` | always built | Math, fixed-step timing, geometry queries, action input, A* navigation, and audio decoding, mixing and device output |
| Assets and scenes | `anima::assets` | `ANIMA_BUILD_ASSETS` (on) | GLB and BC7 KTX2 import, meshes, impostor baking, scenes, objects, components, prefabs, animation, cameras and lighting |
| Desktop | `anima::desktop` | `ANIMA_BUILD_DESKTOP` (on) | SDL3/Vulkan rendering: skinning, materials with anisotropic filtering, application shaders, instanced placements, visibility ranges, levels of detail, octahedral impostors, culling, sky and directional shadows |
| 3D physics | `anima::physics`, `anima::physics_scene` | `ANIMA_BUILD_PHYSICS` | Jolt simulation and queries; the scene target also needs assets |
| 2D physics | `anima::physics2d`, `anima::physics2d_scene` | `ANIMA_BUILD_PHYSICS2D` | Box2D simulation and queries; the scene target also needs assets |
| SDL input | `anima::input_sdl` | `ANIMA_BUILD_INPUT_SDL` | SDL event conversion using SDL headers only |
| UI | `anima::ui_documents`, `anima::ui_scene`, `anima::ui` | `ANIMA_BUILD_UI_DOCUMENTS`, `ANIMA_BUILD_UI` | Headless RmlUi documents, their scene integration, and desktop presentation (requires desktop) |

Jolt, Box2D, miniaudio, meshoptimizer and the importers stay private implementation
dependencies; `anima::ui` exposes RmlUi deliberately for native interoperability. For
audio, `anima::core` links the platform's thread library, and on macOS the Core Audio
frameworks; Linux audio libraries load at run time. The
[consumer suite](tests/consumer) builds each target as an external project with the
engine's tools and tests off, which checks these dependency boundaries.

Box2D is written in C, so a project that links `anima::physics2d` must enable C in
its top-level directory, for example with `project(my_game LANGUAGES C CXX)`.
Packaging FreeType and other shared libraries is up to the application.

## Architecture

Anima supplies reusable mechanisms; applications supply content, rules and policy.

| Engine | Application |
| --- | --- |
| Objects, hierarchy, components, scheduling and prefab construction | Object composition, gameplay components and transitions |
| Scene and component documents with explicit resource resolvers | Save slots, progression, migrations and network authority |
| Meshes, materials, lighting, visibility and rendering | Art direction, world layout, quality settings and gameplay effects |
| Terrain queries, collision bodies, contacts, rays and sweeps | Movement, support rules, damage and collision response |
| Pose evaluation, masked layers, state machines, contacts, action timelines, attachment placement and fitted meshes | Clip, action and handling choice, state machine definitions and parameters, timing rules, item metadata and gameplay outcomes |
| Audio decoding, mixing, voice limits and output, input state and navigation queries | Sound selection and priorities, action responses, destinations and movement authority |
| UI documents, layout, rendering and events | Screens, game data, commands and navigation |
| Import and resource lifetime | Production assets, content recipes and deployment |
| Load staging, cancellation and progress | Load threads, job systems and scheduling |

The animation documents Anima reads, from model manifests and motion contracts to
state machines and action, attachment and fitted catalogs, hold only what the engine
evaluates, under names the application defines. Content such as equipment lists,
slots, categories and review groupings, and rules such as which held item performs
an action or how presentation timing must match gameplay timing, stay in the
application's own data and code.

A `Scene` owns `GameObject`s with transforms and native C++ components, a `Prefab`
builds independent object assemblies through the same APIs, and a `SceneSet` owns
additive scenes. JSON is the storage format, not a second scene implementation:
`ComponentCodecs` maps stable component keys to explicit codecs, and failed decoding
rolls back everything it staged.

Loading splits by thread. GLB import, mesh compilation, texture preparation and
document staging (`stage_scene`, `stage_scene_set`) read only their inputs, so they
may run on any thread. Staging starts no thread: the application runs these calls
on its own threads or job system, then commits each staged document through
`load_scene` or `SceneSet` on the thread that owns its scenes, where objects are
created and components decoded. A `StopSource` cancels staging between steps, and a
`StagingProgress` counts them.

### One frame

Applications own the loop. Call the drivers of the modules you link in this order;
the [runtime consumer](tests/consumer/runtime.hpp) runs it with every optional module:

```cpp
anima::input::begin_frame(scenes);
for (const auto &event : events)
    anima::input::dispatch(scenes, event);
const auto batch = clock.advance(elapsed); // anima::FixedStepClock
const double tick = std::chrono::duration<double>(clock.step()).count();
for (std::uint32_t step = 0; step < batch.steps; ++step) {
    anima::navigation::update_agents(scenes, tick);
    scenes.fixed_update(tick);
    anima::physics::step(scenes, world3d, tick);
    anima::physics2d::step(scenes, world2d, tick);
}
scenes.update(std::chrono::duration<double>(elapsed).count());
anima::synchronize_audio(scenes, audio);
anima::sync_ui_panels(scenes);
// Then draw. An Audio with a device mixes on its own thread; render one without here.
```

Navigation computes each agent's velocity without moving its object; a fixed-update
component or the application applies it. Physics publishes poses after the fixed
callbacks, and audio synchronization reads the final transforms without advancing
sample time. Input edges last until the next `begin_frame`, through frames with
several fixed ticks or none, so queue an edge-triggered command until a tick
consumes it. Delta actions, bound to mouse motion or the scroll wheel, sum what
arrived during one `begin_frame` interval, so read them once per frame, for example
in `update`: read in each fixed tick, they would apply twice in a frame with two
ticks and not at all in a frame with none.

Each driver takes one `Scene` or a whole `SceneSet`, and the set's active scene does
not filter it. A physics world or mixer shared by several scenes needs one call
covering all of them. A driver validates its whole selection before publishing
anything, drivers do not roll each other back, and every driver rejects a selection
that is running component callbacks. Declare services, then persistence
configuration, then scenes, so destruction releases scene-owned resources before
the services they use. Replace or destroy a scene between these calls, not from its
own component callbacks: destroying a scene or scene set while one of its calls is in
progress, or while a driver holds it, terminates the program with a diagnostic.

## Conventions

Coordinates are right-handed with +Y up; cameras and audio listeners face local -Z.
Physics uses caller-consistent units, normally meters and kilograms. `Mat4`
is column-major with column vectors, quaternions are XYZW, and transforms compose as
T * R * S. Projections target Vulkan clip space (Y flipped) with reversed depth, from 1
at the near plane to 0 at the far plane, which keeps a floating-point depth buffer
precise at a distance; `perspective()`, `orthographic()` and `view_matrix()` build
them. Sun shadow regions keep forward depth, from 0 on the side facing the light, since
their orthographic projections space depth evenly.

## API reference

The public headers in `include/anima` are the reference: each declaration documents
its contract. To render them with Doxygen 1.10 or newer:

```sh
cmake -S docs -B build/docs
cmake --build build/docs --target anima_docs
```

Then open `build/docs/reference/html/index.html`. The documentation project needs
no compiler or engine dependencies.

## Development

Public headers live in `include/anima`, implementations in `src`, applications in
`apps` and shaders in `shaders`. Tests, GPU consumers and synthetic fixtures are in
`tests`. Keep generated output in ignored `build` or `out` directories. See
[contributing](CONTRIBUTING.md) and the [security policy](SECURITY.md).

## License

Anima's original code and documentation are licensed under [Apache-2.0](LICENSE).
Third-party dependencies and test assets retain their own licenses; see
[third-party notices](third_party/README.md) and the accompanying license files.
