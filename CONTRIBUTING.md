# Contributing to Anima

Keep changes focused on reusable engine behavior. Applications supply their own
content, gameplay rules, scheduling and deployment policy; see the
[architecture](README.md#architecture). Documents use one current format; do not
add compatibility readers or forwarding APIs for removed contracts.

## Code

Use C++20 and the repository's `.clang-format`; CI rejects first-party sources
that clang-format 22.1.8 would change. Keep backend types private where the public
module promises dependency isolation. Validate inputs before publishing state, and
make resource ownership explicit. A new document field with a natural default may be
omitted, and adding it keeps the version; renaming or removing a field changes it.

Document every public declaration with a `///` comment stating its contract:
ownership and lifetime, threading, units and ranges, failures and their exception
types, and persistence formats. The headers are the API reference, so state only
what the implementation does, and update the comment with the behavior.

## Tests

For a behavior change, include a regression test of the public contract. New and
updated suites use [doctest](third_party/README.md): link `anima_test_main` and
assert each rejection with `CHECK_THROWS_WITH_AS` or `REQUIRE_THROWS_WITH_AS`
against its exact exception type and message. New runtime capabilities should
have an independent consumer in `tests/consumer`.

The `full` preset builds and tests every module:

```sh
cmake --workflow --preset full
```

For an offline build, set `FETCHCONTENT_FULLY_DISCONNECTED=ON` and point
`FETCHCONTENT_SOURCE_DIR_JOLT`, `FETCHCONTENT_SOURCE_DIR_BOX2D`,
`FETCHCONTENT_SOURCE_DIR_RMLUI` and `FETCHCONTENT_SOURCE_DIR_SDL3` at the pinned
sources; an RmlUi override must already carry the patch described in
[third_party/rmlui](third_party/rmlui/README.md). `ANIMA_TEST_GLB` and
`ANIMA_TEST_MANIFEST` add regressions against an exported model and its manifest.

### Benchmarks

`anima_scene_benchmarks` times hierarchy construction, bulk destruction and per-frame
scene work. CTest runs it at 1,024 objects to check each scenario's result. Measure a
Release build at its default 65,536 objects, the scene document limit, and record the
machine and configuration with the numbers:

```sh
cmake -S . -B build/benchmarks -G Ninja -DCMAKE_BUILD_TYPE=Release -DANIMA_BUILD_DESKTOP=OFF
cmake --build build/benchmarks --target anima_scene_benchmarks
build/benchmarks/anima_scene_benchmarks --repetitions 5
```

### Sanitizers

`ANIMA_ENABLE_SANITIZERS=ON` instruments Anima and its source-built dependencies
with AddressSanitizer, plus UndefinedBehaviorSanitizer on Linux and macOS. CI's
sanitizer builds are presets that fetch the pinned SDL. On Linux and macOS they use
Clang in Debug:

```sh
cmake --workflow --preset ci-sanitizers
```

On Windows, run `cmake --workflow --preset ci-sanitizers-msvc` from a Visual Studio
developer environment. It builds `RelWithDebInfo`, because configuration rejects the
MSVC debug options that ASan cannot use.

### GPU and device checks

CTest's `gpu` label checks the renderer and UI on a Vulkan device: `anima_check`
drives the viewer, and `consumer_desktop` and `consumer_ui` render synthetic scenes
through the public API. `consumer_desktop` runs one mode per check: its default
instances check, `--allocations`, `--culling`, `--foliage`, `--environment`,
`--resources`, `--replace`, `--material-sampling`, `--pbr`, `--surface-maps`,
`--sidedness`, `--blending`, which compares blended materials with the renderer's
documented compositing, fog, exposure, draw order and shadow policy,
`--custom-materials`, which draws the consumer's own water, effect and probe shaders,
compiled by glslc in its build, and compares their frame inputs, passes, sorting, time,
opaque depth and color, skinning and shadows with the renderer's contract,
`--texture-memory`, which measures the CPU texels and device images of a 2048-texel
texture and checks that meshes and custom materials compiled with
`TexelRetention::until_upload` let their texels go once uploaded and keep drawing, and
`--compressed-textures`, which draws BC7 images from `tests/textures` within their
encoding error of their sources, compares the device's BC7 sampling with the CPU
decoding that devices without it get, and measures the memory of both formats.
Each check opens a window, requires Khronos validation, compares its frames in memory,
and fails on a pixel mismatch or any validation message. The checks run one at a time:

```sh
ctest --preset full -L gpu
```

A failed comparison writes the images it compared, as PNG files, into the check's
directory under `gpu-checks` in the build tree; a passing check writes no image.
Without a display or a Vulkan device, each check exits with 77, which CTest reports
as skipped; set `ANIMA_REQUIRE_GPU=1` to make that a failure. With several Vulkan
drivers installed, choose one with `VK_DRIVER_FILES`. `ctest -L gpu -N -V` shows
each check's command, which also runs on its own.

CI runs the label on Linux with Mesa's lavapipe, a software Vulkan driver, under a
virtual X display. That checks rendering logic and validation on every pull request,
but not a hardware driver: for renderer or UI changes, also run the label on a GPU,
and report the device and driver used, and any platform left untested, in the pull
request. The checks minimize and restore windows, which X11 leaves to the window
manager, so a virtual display needs one running; CI starts Openbox.

Audio tests render offline or play through miniaudio's null backend, so they
produce no sound and measure no device latency. Physical input devices, text input
methods and display scaling also need checks on real hardware.

## Documentation

The API reference renders from the headers; Doxygen warnings fail the build:

```sh
cmake -S docs -B build/docs
cmake --build build/docs --target anima_docs
```

## Dependencies

Jolt, Box2D and RmlUi are fetched from pinned archives, and SDL, when fetched, from
a pinned release commit. Each pin is in its `cmake/Anima*.cmake` file and recorded
in its notes under [third_party](third_party/README.md). To update one:

1. Point the URL or `GIT_TAG` at the new release, keeping the release name in a
   comment beside a commit, and replace `URL_HASH` with the SHA-256 of the new
   archive (`cmake -E sha256sum` on the downloaded file).
2. Update the release, revision and hash in its notes, its license file if the
   license changed, and any patch it carries; RmlUi's patch must still apply.
3. Read the upstream release notes for API and behavior changes.
4. Run the `full` workflow and the CI presets that fetch the dependency
   (`ci-full-release` and `ci-sanitizers`), and the `gpu` label when SDL or RmlUi
   changes.

Vendored headers under `third_party` are replaced whole at the new revision: update
their table and SHA-256 list in [third_party/README.md](third_party/README.md) and
reapply the local changes it records. GitHub Actions are pinned by commit with a
version comment, and Dependabot proposes their updates.

## Pull requests

CI builds headless, full and sanitizer configurations on Linux, macOS and Windows,
plus an optimized full build on Linux, and runs every test. Each of these legs is a
preset, so `cmake --workflow --preset <preset>` reproduces it: `ci-headless-debug`,
`ci-full-release` and the others, and `full` itself on macOS. CI also lints the
workflows, checks formatting, scans the Git history for secrets, and runs CodeQL
and the Doxygen build; `CI required` gates merging. CodeQL runs its
`security-extended` queries on the workflows and on C++. It analyzes C++ from a
traced build of every module, because extraction without a build infers compiler
flags and include paths, which is less accurate for code with many external
dependencies. Results in fetched dependencies and vendored headers are dropped
before upload. Record the commands and results of checks CI cannot run, such as
GPU and device checks, in the pull request.

Submit only material you are authorized to contribute. Contributions to Anima's
original code and documentation use [Apache-2.0](LICENSE); preserve third-party
notices and licenses when modifying dependency integration or test assets.

Report suspected vulnerabilities through the [security policy](SECURITY.md). Keep
credentials in local ignored files and check staged changes before pushing.
