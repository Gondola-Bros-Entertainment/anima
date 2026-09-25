# Anima

Anima is a modular C++20 game engine. It provides a headless runtime for scenes,
animation, physics, audio, input and navigation, with an optional SDL3/Vulkan
renderer and RmlUi interface layer. Applications compose the libraries they need
through public CMake targets.

The engine is under active development. Its runtime has independent consumers
and automated checks, but it does not yet provide a complete editor, project
manager or standalone player workflow. The subsystem guides describe supported
behavior and current limitations.

## Build

The headless runtime requires CMake 3.25+ and a C++20 compiler. The default test
configuration also requires Python 3.10+ for repository checks:

```sh
git clone https://github.com/Gondola-Bros-Entertainment/anima.git
cd anima
cmake --preset headless
cmake --build --preset headless
ctest --preset headless --output-on-failure
```

Use the `core` preset for the dependency-free library. To build the desktop viewer,
install SDL3 and a Vulkan SDK with `glslc`, then run:

```sh
cmake --preset desktop
cmake --build --preset desktop
ctest --preset desktop --output-on-failure
./build/desktop/anima --validation
```

Validation mode requires the Vulkan validation layer. UI support additionally
requires FreeType. The [build guide](docs/engine.md) covers platform setup,
source-built dependencies, viewer controls and optional exported-model checks.
Ordinary tests use synthetic fixtures and do not open a window or require Blender.
GPU checks run separately.

## Use Anima in a project

Add a pinned checkout as a CMake dependency and link the required targets:

```cmake
set(ANIMA_BUILD_TOOLS OFF)
set(ANIMA_BUILD_TESTS OFF)
set(ANIMA_BUILD_DESKTOP OFF)
add_subdirectory(external/anima)
target_link_libraries(my_application PRIVATE anima::assets)
```

The [consumer guide](docs/consuming-anima.md) documents configuration, public
headers and resource ownership. The [independent consumers](tests/consumer)
exercise these interfaces with the engine's tools and tests disabled.

| Module | Public targets | Capabilities |
| --- | --- | --- |
| Core | `anima::core` | Math, fixed-step timing, geometry queries, action input, A* navigation and buffered audio mixing |
| Assets and scenes | `anima::assets` | GLB import, shared meshes, objects/components, hierarchy, additive scenes, prefabs, animation, cameras and directional lighting |
| 3D physics | `anima::physics`, `anima::physics_scene` | Optional Jolt simulation, checked bodies, hulls/compounds, queries and scene/prefab integration |
| 2D physics | `anima::physics2d`, `anima::physics2d_scene` | Optional Box2D simulation, checked bodies, queries and scene/prefab integration |
| SDL adapters | `anima::input_sdl`, `anima::audio_output` | Independent input-event conversion and audio-device output |
| Desktop | `anima::desktop` | Vulkan mesh rendering, skinning, materials, frustum culling, gradient sky and directional shadows |
| UI | `anima::ui_documents`, `anima::ui` | Headless RmlUi documents and optional desktop presentation |

Physics backends remain private implementation dependencies. Core-only consumers
do not configure them. Audio output requires SDL but does not require Vulkan,
assets or desktop rendering. The [engine boundaries](docs/engine-boundaries.md)
describe how applications supply content, policy and scheduling.

## Documentation

- [Scenes, components and persistence](docs/scene-objects.md)
- [Cameras](docs/cameras.md), [lighting](docs/lighting.md) and [rendering](docs/rendering-architecture.md)
- [Animation](docs/animation-evaluation.md) and [asset reimport](docs/asset-reimport.md)
- [3D physics](docs/physics.md) and [2D physics](docs/physics2d.md)
- [Audio](docs/audio.md), [input](docs/input.md), [navigation](docs/navigation.md) and [UI](docs/ui-integration.md)
- [Local qualification](docs/local-qualification.md) and [API reference generation](docs/documentation.md)

## Development

Public headers live in `include/anima`, implementations in `src`, applications
in `apps`, and shaders in `shaders`. Tests and synthetic fixtures are in `tests`;
GPU verification tools are in `tools/engine`. Applications own their asset
authoring and export pipelines; Anima imports and runs the resulting resources.

macOS, Windows and Linux are development targets. Headless compiler checks and
GPU/device qualification are separate: a successful build does not establish
physical-device support. See [contributing](CONTRIBUTING.md) for the local workflow.

Keep generated output in ignored `build` or `out` directories. GPU runners delete
readbacks after successful comparisons; remove failed captures after investigation.
Check documentation links with `python3 tools/check_repository.py`.

## License

Anima's original code and documentation are licensed under [Apache-2.0](LICENSE).
Third-party dependencies and test assets retain their own licenses; see
[third-party notices](third_party/README.md) and the accompanying license files.
