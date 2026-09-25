# Local qualification

Qualify engine changes with local compiler and runtime checks. macOS, native
Windows/MSVC and Linux/GCC are separate environments. A Linux container provides
compiler/runtime evidence; it does not replace physical GPU, audio or input-device
testing, packaging checks or sustained application workloads.

Use these checks alongside CI, especially for optional modules and physical
devices that hosted runners do not cover.

## Continuous integration

The [CI workflow](../.github/workflows/ci.yml) runs on pushes to `main`, pull
requests and manual dispatch. After the short repository checks, platform builds,
CodeQL and documentation run in parallel. The final `CI required` check fails if
any required job fails, is cancelled or is skipped, providing one stable check
for branch rules.

CI covers Debug and Release headless builds on Linux, macOS and Windows,
including copied core/asset consumers. Each platform also builds the optional
libraries, Vulkan renderer, shaders, viewer and UI together in Debug. Those jobs
check Jolt, Box2D, RmlUi documents, SDL input conversion and dummy-device audio
output, along with their independent consumers. They compile the independent
UI/desktop API consumer without executing the two consumers that open Vulkan
windows.

Linux Clang and macOS AppleClang Debug jobs check the headless runtime, optional
libraries, bundled parsers and copied consumers under AddressSanitizer and
UndefinedBehaviorSanitizer. Linux also enables LeakSanitizer. Findings fail the
job. Windows also checks the optional runtime and consumers with native MSVC
AddressSanitizer in RelWithDebInfo. MSVC does not provide UndefinedBehaviorSanitizer.

Linux/macOS builds use Ninja and a bounded compiler cache. Windows caches vcpkg
dependency binaries and uses Visual Studio. Cache hits never skip configuration,
linking or tests. Two consumer builds run concurrently, with two compiler
processes each; ordinary runtime tests run in parallel. Compiler cache
statistics in each job distinguish cold runs from subsequent cache reuse.

Repository checks validate documentation links, lint workflow syntax and shell
commands with actionlint, and scan the complete fetched Git history with Gitleaks.
Both tools use checksum-verified releases. A generated detection canary verifies
the secret scanner before the real scan; findings are redacted in logs.
Dependabot proposes weekly updates to the SHA-pinned GitHub Actions.

The reusable [CodeQL workflow](../.github/workflows/codeql.yml) scans Actions,
C/C++ and Python with the security-extended query suite. Its C++ analysis uses
`build-mode: none` to include optional modules. It is required by PR/push CI and
also runs weekly. The documentation stage generates Doxygen HTML/XML. CI does
not qualify physical GPUs, speakers or input devices. Use the relevant local
checks for those paths.

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
dependency through public targets, with engine tools/tests disabled. They forward
the selected compilers and `ANIMA_ENABLE_SANITIZERS`, including instrumentation of
the consumer executable. Arbitrary parent compiler flags are not forwarded.
Optional-module selection determines the test count.

## Address and undefined-behavior sanitizers

`ANIMA_ENABLE_SANITIZERS` enables ASan and UBSan with GCC/Clang on Linux/macOS,
and ASan with native MSVC on Windows. Use a separate Debug build on Linux/macOS:

```sh
CC=clang CXX=clang++ cmake --preset headless -B build/sanitizers \
  -DANIMA_ENABLE_SANITIZERS=ON
cmake --build build/sanitizers --parallel 2
ASAN_OPTIONS=halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build/sanitizers --output-on-failure --no-tests=error
```

On Linux, add `detect_leaks=1:strict_string_checks=1` to `ASAN_OPTIONS`. Leave leak
detection at its runtime default for macOS checks. Enable relevant optional
modules with the same flags and dependency settings as the runtime build above.
CI runs these checks on Linux with Clang and macOS with AppleClang, including
the optional runtime modules.

The option instruments Anima libraries, tools, tests, copied consumer executables,
vendored image/GLB parsers, and source-built Jolt, Box2D, RmlUi and SDL targets.
Installed packages such as FreeType or SDL remain uninstrumented. On Linux/macOS,
it retains frame pointers and makes detected undefined behavior terminate the
process so CTest reports a failure. Compilation flags stay private
to the instrumented targets; their required sanitizer runtime link options reach
the final executable. No global compiler flags or dependency ABI macros change.
The option is off by default and rejects unsupported compilers/platforms at
configuration time.

For Windows/MSVC, use a separate RelWithDebInfo configuration:

```powershell
cmake -S . -B build/sanitizers -A x64 `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_CONFIGURATION_TYPES=RelWithDebInfo `
  -DANIMA_BUILD_DESKTOP=OFF -DANIMA_ENABLE_SANITIZERS=ON
cmake --build build/sanitizers --config RelWithDebInfo --parallel 4
ctest --test-dir build/sanitizers -C RelWithDebInfo --output-on-failure --no-tests=error
```

Run from the matching Visual Studio developer environment so the ASan runtime DLL
is on `PATH`. MSVC's default Debug `/RTC` checks and Edit-and-Continue `/ZI` are
incompatible with ASan; configuration rejects them instead of changing unrelated
compiler flags. The sanitizer targets disable incremental linking. Windows CI uses
`ASAN_OPTIONS=continue_on_error=0:strict_string_checks=1:alloc_dealloc_mismatch=1`;
Linux leak options and MSVC-unsupported `halt_on_error` are not set there.

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
