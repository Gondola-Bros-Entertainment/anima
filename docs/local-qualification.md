# Local qualification

Qualify engine changes with local compiler and runtime checks. macOS, native
Windows/MSVC and Linux/GCC are separate environments. A Linux container provides
compiler/runtime evidence; it does not replace physical GPU, audio or input-device
testing, packaging checks or sustained application workloads.

Use these checks alongside CI, especially for optional modules and physical
devices that hosted runners do not cover.

## Continuous integration

The [CI workflow](../.github/workflows/ci.yml) runs on pushes to `main`, pull
requests and manual dispatch. Its stages run in order: repository and package
checks, platform builds, optional libraries, then generated documentation. A failed
stage stops later stages. The platform matrix runs at most three jobs at once.

CI covers Debug and Release headless builds on Linux, macOS and Windows,
including copied core/asset consumers. A Linux job
also enables Jolt, Box2D, RmlUi documents, SDL input conversion and dummy-device
audio output, with their applicable independent consumers.

Separate jobs check Python 3.10 and 3.14 authoring contracts in normal and
optimized modes, source/wheel license metadata, documentation links and Doxygen
HTML/XML generation. CI does not run Blender, compile the Vulkan desktop renderer,
or qualify physical GPUs, speakers or input devices. Use the relevant local checks
for those paths.

## Source and dependencies

Build the exact intended revision in a clean engine checkout. Record its full
commit ID, compiler version, configuration and commands alongside the results.
Use a separate source/build directory for each platform. Do not overwrite local
changes when updating a qualification checkout.

The optional Jolt, Box2D and RmlUi integrations use pinned sources. For offline
builds, prepare those exact sources in advance, including the documented RmlUi
patch, and provide CMake source overrides. Keep the public dependency pins intact.
Use process-local compiler/toolchain settings; qualification should not require
global environment changes or installing dependencies during the test run.

## Headless runtime checks

An example Linux/macOS configuration with both physics backends, UI documents
and the SDL input converter is:

```sh
cmake -S . -B build/runtime \
  -DCMAKE_BUILD_TYPE=Debug \
  -DANIMA_BUILD_DESKTOP=OFF \
  -DANIMA_BUILD_PHYSICS=ON \
  -DANIMA_BUILD_PHYSICS2D=ON \
  -DANIMA_BUILD_UI_DOCUMENTS=ON \
  -DANIMA_BUILD_INPUT_SDL=ON \
  -DANIMA_INPUT_SDL_INCLUDE_DIR=/path/to/sdl/include \
  -DANIMA_WARNINGS_AS_ERRORS=ON \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DFETCHCONTENT_SOURCE_DIR_JOLT=/path/to/jolt \
  -DFETCHCONTENT_SOURCE_DIR_BOX2D=/path/to/box2d \
  -DFETCHCONTENT_SOURCE_DIR_RMLUI=/path/to/rmlui
cmake --build build/runtime --parallel 2
ctest --test-dir build/runtime --output-on-failure --no-tests=error
```

The paths are explicit local dependency locations. UI documents require FreeType.
The SDL input converter requires SDL headers but no SDL runtime or graphics
libraries. On Windows, use the selected MSVC generator/toolchain and pass
`--config Release` to the build and `-C Release` to CTest for a multiconfiguration
generator. Keep dependency linkage and configuration consistent.

CTest includes copied consumer projects. They configure Anima as an external
dependency through public targets, with engine tools/tests disabled. These builds
do not inherit parent sanitizer flags; report their results separately from
instrumented engine checks. Optional-module selection determines the test count.

## SDL audio output

`ANIMA_BUILD_AUDIO_OUTPUT=ON` adds output checks and an independent consumer without
requiring assets, desktop rendering, Vulkan, physics or UI in that consumer.
Device checks explicitly select SDL's dummy driver. They do not qualify speakers,
latency, device discovery or recovery.

For an offline source build, prepare the pinned SDL 3.4.2 revision
`683181b47cfabd293e3ea409f838915b8297a4fd` and set `ANIMA_FETCH_SDL=ON` with
`FETCHCONTENT_SOURCE_DIR_SDL3=/path/to/sdl`. Audio-only builds can set `SDL_VIDEO`,
`SDL_RENDER`, `SDL_GPU` and `SDL_CAMERA` to `OFF`, with `SDL_STATIC=ON` and
`SDL_SHARED=OFF`. On Linux, also set `SDL_UNIX_CONSOLE_BUILD=ON`. The copied
consumer forwards the prepared source and selected linkage mode.

## Desktop and device checks

Use the desktop preset to build the renderer and applicable UI targets. Run the
relevant independent GPU consumers separately from CTest, on an available display
with Vulkan validation enabled. The [build guide](engine.md) and subsystem guides
document their commands and assertions.

Record the OS, physical device/backend, drawable dimensions and enabled features.
Report simulated failures separately from actual device failures. Performance runs
should use optimized builds, fixed workloads and warmup/sample counts; keep them
separate from capture/validation runs and concurrent builds.

Successful and investigated failure readbacks should be deleted, never archived
or compressed. Keep small text/JSON evidence in ignored build output directories.

## Evidence and review

Before integration, record the qualified revision, local commands and results,
independent-consumer coverage, relevant sanitizer checks, review findings and
remaining limitations. Rebuild after behavior changes; do not report retained
evidence as a fresh run. Compiler checks, synthetic fixtures and local GPU checks
establish bounded contracts rather than general production or release readiness.
