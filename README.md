# Anima

Anima is a modular C++20 game engine. It provides a headless runtime for scenes,
animation, physics, audio, input and navigation, with an optional SDL3/Vulkan
renderer and RmlUi interface layer. Applications compose the libraries they need
through public CMake targets and run their own frame loop.

The engine is under active development. It has no editor, project manager or
standalone player.

## Requirements

- CMake 3.25 or newer and a C++20 compiler. The presets use Ninja.
- `anima::desktop` builds on Windows, macOS and Linux. It needs SDL3 3.2 or newer
  (installed, or fetched with `ANIMA_FETCH_SDL=ON`), the Vulkan loader, `glslc`,
  and Vulkan headers that declare `VK_EXT_surface_maintenance1` and
  `VK_EXT_swapchain_maintenance1`. At run time the renderer needs a Vulkan 1.1
  device, through MoltenVK on macOS.
- The UI modules need FreeType.
- The 2D physics modules need a C17 compiler for Box2D.
- The vector modules need a macOS deployment target of 13.3 or later, for Linea.

Enabled modules fetch Jolt Physics, Box2D, RmlUi and Linea at configure time with
FetchContent; the [third-party notes](third_party/README.md) record each pinned
version, and Linea follows its `main` branch.

Tested in CI with GCC on Ubuntu 24.04, Apple Clang on macOS 15 and MSVC on Windows.

## Use Anima in a project

Add a pinned checkout with `add_subdirectory`, or a pinned commit with
`FetchContent_MakeAvailable`, and link the targets you need. Set options before
adding it:

```cmake
set(ANIMA_BUILD_PHYSICS ON)
add_subdirectory(external/anima)

add_executable(my_game main.cpp)
target_link_libraries(my_game PRIVATE anima::desktop anima::physics_scene)
anima_copy_sdl_runtime(my_game)
```

`anima_copy_sdl_runtime` copies a shared SDL3 library beside the executable on
Windows and does nothing elsewhere.

| Module | Targets | Option | Provides |
| --- | --- | --- | --- |
| Core | `anima::core` | always built | Math, fixed-step timing, geometry queries, action input, A* navigation, and audio decoding, mixing and device output |
| Assets and scenes | `anima::assets` | `ANIMA_BUILD_ASSETS` (on) | GLB and BC7 KTX2 import, meshes, impostor baking, sprite sheets, scenes, objects, components, prefabs, animation, cameras and lighting |
| Desktop | `anima::desktop` | `ANIMA_BUILD_DESKTOP` (on) | SDL3/Vulkan rendering: skinning, materials with anisotropic filtering, application shaders, batched sprites, instanced placements, visibility ranges, levels of detail, octahedral impostors, culling, a physically based sky and atmosphere, height fog, directional shadows and bloom |
| 3D physics | `anima::physics`, `anima::physics_scene` | `ANIMA_BUILD_PHYSICS` (off) | Jolt simulation and queries; the scene target also needs assets |
| 2D physics | `anima::physics2d`, `anima::physics2d_scene` | `ANIMA_BUILD_PHYSICS2D` (off) | Box2D simulation and queries; the scene target also needs assets |
| SDL input | `anima::input_sdl` | `ANIMA_BUILD_INPUT_SDL` (off) | SDL event conversion using SDL headers only |
| UI | `anima::ui_documents`, `anima::ui_scene`, `anima::ui` | `ANIMA_BUILD_UI_DOCUMENTS`, `ANIMA_BUILD_UI` (off) | Headless RmlUi documents, their scene integration (needs assets), and desktop presentation (needs desktop) |
| Vector art | `anima::vector`, `anima::vector_ui` | `ANIMA_BUILD_VECTOR` (off) | Linea drawings rendered into a sprite sheet at the display's resolution (needs assets), and registered as UI images (needs UI) |

| Option | Default | Effect |
| --- | --- | --- |
| `ANIMA_FETCH_SDL` | Off | Builds the pinned SDL release instead of finding an installed SDL3 |
| `ANIMA_BUILD_TOOLS` | On when Anima is the top-level project | Builds the `anima` viewer and the `anima_headless` and `anima_inspect` tools |
| `ANIMA_BUILD_TESTS` | On when Anima is the top-level project and `BUILD_TESTING` is on | Builds and registers the tests |
| `ANIMA_WARNINGS_AS_ERRORS` | Off; the presets turn it on | Fails the build on warnings in Anima's own targets |
| `ANIMA_INPUT_SDL_INCLUDE_DIR` | The fetched SDL's, or searched for | Directory containing `SDL3/SDL_events.h` for `anima::input_sdl` |

Turn `ANIMA_BUILD_DESKTOP` off for a headless project, and `ANIMA_BUILD_ASSETS` as
well for core only; such projects configure no SDL, Vulkan or RmlUi.

Jolt, Box2D, Vulkan, miniaudio, meshoptimizer and the importers are private
dependencies. `anima::desktop` exposes SDL3, since the application creates the
window the renderer draws into; `anima::ui` exposes RmlUi for native
interoperability; and `anima::vector` exposes Linea, whose drawings describe the
art it renders. For audio, `anima::core` links the platform's thread library, and
on macOS the Core Audio frameworks; Linux audio libraries load at run time.

Box2D is written in C, so a project that links `anima::physics2d` must enable C in
its top-level directory, for example with `project(my_game LANGUAGES C CXX)`.
Packaging FreeType and other shared libraries is up to the application.

## Frame loop

Applications own the loop. Call the drivers of the modules you link in this order;
the [runtime consumer](tests/consumer/runtime.hpp) runs the same loop without the
UI step:

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
anima::synchronize_ui_panels(scenes);
// Then draw. An Audio with a device mixes on its own thread; render one without here.
```

Navigation computes each agent's velocity without moving its object; a fixed-update
component or the application applies it. Physics writes poses back after the fixed
callbacks, and audio synchronization reads the final transforms without advancing
sample time. Input edges last until the next `begin_frame`, through frames with
several fixed ticks or none, so queue an edge-triggered command until a tick
consumes it. Delta actions, bound to mouse motion or the scroll wheel, sum one
`begin_frame` interval, so read them once per frame, for example in `update`, not
in each fixed tick.

Each driver takes one `Scene` or a whole `SceneSet`, and the set's active scene
does not filter it, so a physics world or mixer shared by several scenes needs one
call covering all of them. A driver validates its whole selection before changing
any of it, but drivers do not roll each other back, and every driver rejects a
selection that is running component callbacks. Replace or destroy a scene between
these calls: destroying a scene or scene set while one of its calls is in progress,
or while a driver holds it, terminates the program with a diagnostic.

## Scenes and loading

A `Scene` owns `GameObject`s with transforms and native C++ components, a `Prefab`
builds independent object assemblies through the same APIs, and a `SceneSet` owns
additive scenes. JSON is the storage format: `ComponentCodecs` maps stable component
keys to explicit codecs, and failed decoding rolls back everything it staged.

GLB import, mesh compilation, texture preparation and document staging
(`stage_scene`, `stage_scene_set`) read only their inputs, so they may run on any
thread. Staging starts no thread: run these calls on your own threads or job
system, then commit each staged document through `load_scene` or `SceneSet` on the
thread that owns its scenes. A `StopSource` cancels staging between steps, and a
`StagingProgress` counts them.

## Conventions

Coordinates are right-handed with +Y up; cameras and audio listeners face local -Z.
Physics uses caller-consistent units, normally meters and kilograms. `Mat4` is
column-major with column vectors, quaternions are XYZW, and transforms compose as
T * R * S. Projections target Vulkan clip space (Y flipped) with reversed depth,
from 1 at the near plane to 0 at the far plane; `perspective()`, `orthographic()`
and `Camera::projection()` build them, and `resolve_camera()` composes the selected
camera's with its view. The sun's shadow cascades and detail region keep forward
depth, from 0 on the side facing the light.

## Build and test

Each workflow preset configures, builds and tests one configuration: `core` builds
only `anima::core`, `headless` adds assets and scenes, `desktop` adds the renderer
and the `anima` viewer, and `full` builds every module.

```sh
git clone https://github.com/Gondola-Bros-Entertainment/anima.git
cd anima
cmake --workflow --preset headless
```

```sh
cmake --workflow --preset desktop
./build/desktop/anima --validation
```

`--validation` requires the Vulkan validation layer, and `anima --help` lists the
viewer's options. Tests use synthetic fixtures. Only the `gpu` label's checks open
a window, and each is skipped without a display or Vulkan device. See
[contributing](CONTRIBUTING.md) for sanitizers, GPU checks, benchmarks and
offline builds.

## API reference

The public headers in `include/anima` are the reference: each declaration documents
its contract. To render them with Doxygen 1.10 or newer:

```sh
cmake -S docs -B build/docs
cmake --build build/docs --target anima_docs
```

Then open `build/docs/reference/html/index.html`. The documentation project needs
no compiler or engine dependencies.

## Contributing

See [contributing](CONTRIBUTING.md), and report vulnerabilities through the
[security policy](SECURITY.md).

## License

MIT. See [LICENSE](LICENSE). Third-party dependencies and test assets retain their
own licenses; see [third-party notices](third_party/README.md) and the accompanying
license files.
