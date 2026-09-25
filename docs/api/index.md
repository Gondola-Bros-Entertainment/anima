# Anima C++ reference

Anima is a reusable C++20 runtime. Games own rules, content, authored appearance,
saves and network authority. Public headers are the reference source; private
implementation headers and third-party libraries are excluded from this site.

This reference includes an inventory of public declarations. **An extracted
declaration is not a documented or qualified contract.** Use the subsystem guides in the repository's
[documentation directory](https://github.com/Gondola-Bros-Entertainment/anima/tree/main/docs).

Start with anima::FixedStepClock for dependency-free simulation timing or
anima::VulkanRenderer for the current desktop renderer's ownership and failure
rules. The renderer remains a fixed pipeline; custom shaders/material programs
and render-pass extensions are not accepted.

For composition, anima::Scene owns objects and native components;
anima::Prefab constructs independent object assemblies. Persistence uses explicit
component codecs and resource resolvers. Runtime objects are separate from their
versioned JSON storage documents.
anima::Camera and anima::CameraView provide scene-facing lenses and explicit view
selection; anima::view_matrix resolves their current transforms for the renderer.

The [engine boundary guide](https://github.com/Gondola-Bros-Entertainment/anima/blob/main/docs/engine-boundaries.md)
describes engine/application ownership. Optional module targets and dependency
requirements are in the
[consumption guide](https://github.com/Gondola-Bros-Entertainment/anima/blob/main/docs/consuming-anima.md).
