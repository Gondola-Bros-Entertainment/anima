# Contributing to Anima

Keep changes focused on reusable engine behavior. Applications supply their own
content, gameplay rules, scheduling and deployment policy. Read the relevant
subsystem guide and [engine boundaries](docs/engine-boundaries.md) before changing
a public contract.

Use C++20 and the repository's `.clang-format`. Keep backend types private where
the public module promises dependency isolation. Validate inputs before publishing
state, make resource ownership explicit, and document exceptions and rollback.
Scene and prefab documents use one current format; do not add compatibility
readers or forwarding APIs for removed contracts.

For a behavior change, include a regression that tests the public contract.
New runtime capabilities should have an independent consumer. Update the relevant
guide and public API comments together. Documentation should describe current
behavior; issue discussions hold proposals and delivery plans.

Run the applicable local build and tests:

```sh
cmake --preset headless
cmake --build --preset headless
ctest --preset headless --output-on-failure
python3 tools/check_repository.py
```

Enable optional modules affected by the change. Renderer changes also need the
appropriate GPU consumer checks. Record the revision, compiler, configuration,
commands and results in the pull request, including platform or hardware gaps.
See [local qualification](docs/local-qualification.md) for the workflow.

Pull requests run the repository's CI checks. Run the relevant checks locally
before submitting a change, and include any platform or device checks that CI
does not cover. Generated builds, logs and captures belong outside the tracked
source tree.

Submit only material you are authorized to contribute. Contributions to Anima's
original code and documentation use [Apache-2.0](LICENSE); preserve third-party
notices and licenses when modifying dependency integration or test assets.
