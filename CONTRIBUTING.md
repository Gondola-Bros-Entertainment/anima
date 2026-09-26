# Contributing to Anima

Keep changes focused on reusable engine behavior. Applications supply their own
content, gameplay rules, scheduling and deployment policy; see the
[architecture](README.md#architecture). Scene and prefab documents use one current
format; do not add compatibility readers or forwarding APIs for removed contracts.

## Code

Use C++20 and the repository's `.clang-format`; CI rejects first-party sources
that clang-format 22.1.8 would change. Keep backend types private where the public
module promises dependency isolation. Validate inputs before publishing state, and
make resource ownership explicit.

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

The presets enable neither physics nor UI. To build and test every module:

```sh
cmake -S . -B build/full -G Ninja -DCMAKE_BUILD_TYPE=Debug -DANIMA_BUILD_TESTS=ON \
  -DANIMA_WARNINGS_AS_ERRORS=ON -DANIMA_BUILD_PHYSICS=ON -DANIMA_BUILD_PHYSICS2D=ON \
  -DANIMA_BUILD_UI_DOCUMENTS=ON -DANIMA_BUILD_UI=ON -DANIMA_BUILD_INPUT_SDL=ON \
  -DANIMA_BUILD_AUDIO_OUTPUT=ON
cmake --build build/full
ctest --test-dir build/full --output-on-failure
```

For an offline build, set `FETCHCONTENT_FULLY_DISCONNECTED=ON` and point
`FETCHCONTENT_SOURCE_DIR_JOLT`, `FETCHCONTENT_SOURCE_DIR_BOX2D`,
`FETCHCONTENT_SOURCE_DIR_RMLUI` and `FETCHCONTENT_SOURCE_DIR_SDL3` at the pinned
sources; an RmlUi override must already carry the patch described in
[third_party/rmlui](third_party/rmlui/README.md). `ANIMA_TEST_GLB` and
`ANIMA_TEST_MANIFEST` add regressions against an exported model and its manifest.

### Sanitizers

`ANIMA_ENABLE_SANITIZERS=ON` instruments Anima and its source-built dependencies
with AddressSanitizer, plus UndefinedBehaviorSanitizer on Linux and macOS. Use a
separate Debug build:

```sh
CC=clang CXX=clang++ cmake --preset headless -B build/sanitizers -DANIMA_ENABLE_SANITIZERS=ON
cmake --build build/sanitizers
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build/sanitizers --output-on-failure
```

On Windows, use a separate `RelWithDebInfo` configuration from a Visual Studio
developer environment; configuration rejects the MSVC debug options that ASan
cannot use.

### GPU and device checks

CTest never creates a Vulkan device. For renderer or UI changes, run `anima_check`,
`consumer_ui --ui` or the matching `consumer_desktop` mode (`--culling`,
`--environment`, `--foliage`, `--resources` or `--replace`), each with an output
directory, on a display with Vulkan validation. Each exits non-zero when a check fails or a
validation message appears. With several Vulkan drivers installed, choose one with
`VK_DRIVER_FILES`. In the pull request, report the device and driver used and any
platform left untested.

Audio tests use SDL's dummy driver, so they produce no sound and measure no
latency. Physical input devices, text input methods and display scaling also need
checks on real hardware.

## Documentation

The API reference renders from the headers; Doxygen warnings fail the build:

```sh
cmake -S docs -B build/docs
cmake --build build/docs --target anima_docs
```

## Pull requests

CI builds headless, full and sanitizer configurations on Linux, macOS and Windows
and runs every test. It also lints the workflows, checks formatting, scans the Git
history for secrets, and runs CodeQL and the Doxygen build; `CI required` gates
merging. CodeQL analyzes C++ from a traced build of every module, because extraction
without a build infers compiler flags and include paths, which is less accurate for
code with many external dependencies. Results in fetched dependencies and vendored
headers are dropped before upload. Record the commands and results of checks CI
cannot run, such as GPU and device checks, in the pull request.

Submit only material you are authorized to contribute. Contributions to Anima's
original code and documentation use [Apache-2.0](LICENSE); preserve third-party
notices and licenses when modifying dependency integration or test assets.

Report suspected vulnerabilities through the [security policy](SECURITY.md). Keep
credentials in local ignored files and check staged changes before pushing.
