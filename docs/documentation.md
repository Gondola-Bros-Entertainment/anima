# Public API documentation

Use Doxygen comments beside public C++ declarations for API contracts. Use
Markdown guides for subsystem architecture and examples. Keep tests and copied
independent consumers as executable evidence of the described contracts.
Generated HTML/XML is build output; do not commit it.

## Local build

Install Doxygen 1.10 or newer separately, then run:

```sh
cmake -S docs -B build/docs
cmake --build build/docs --target anima_docs
```

Open `build/docs/reference/html/index.html`. This standalone project requires
neither a C++ compiler nor SDL, Vulkan, physics or a game checkout. It performs no
downloads. `-DDOXYGEN_EXECUTABLE=/path/to/doxygen` selects a local tool binary.
An engine build may instead enable `ANIMA_BUILD_DOCS=ON`; its `anima_docs` target
writes to that build's `docs/reference` directory. Documentation remains optional
for engine consumers and is not added to the default build.

`EXTRACT_ALL` includes every public declaration, including those without contract
comments. Malformed documentation and invalid references fail the build;
undocumented-declaration warnings are disabled. Use the subsystem guides and
public comments together.

## What to document

Use `///` or `/** ... */` comments. Explain the behavior callers need to rely on:

- Ownership and lifetime: borrowed objects, shared resources and invalidation.
- Thread and frame boundaries; callback reentrancy and permitted mutation.
- Units, coordinate systems, valid ranges and capacity limits.
- Failure and rollback: exception types, preserved state and fatal states.
- Persistence: stable type/version keys and what transient state is omitted.
- Module dependencies and a minimal independent consumer.

Do not restate a method's name or promise behavior the implementation and tests do
not support. Avoid undocumented game assumptions in an apparently generic API.
For example, a mesh API must not silently require all triangles to be horizontal.

Keep architecture and behavioral contracts in these guides. Keep delivery status,
priorities and qualification revisions in issues/PRs. Changes to an API should
update its comments, relevant guide and applicable contract test together.

The [Doxygen comment guide](https://www.doxygen.nl/manual/docblocks.html) describes
the syntax. [Engine boundaries](engine-boundaries.md) and
[rendering architecture](../include/anima/desktop/vulkan_renderer.hpp) document Anima's ownership
decisions. Automated documentation is useful evidence and navigation; dependency
isolation, independent consumers and review enforce the boundary in code.
