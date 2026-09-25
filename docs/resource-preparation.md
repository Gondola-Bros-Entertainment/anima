# Prepare GPU assets independently of scene selection

`VulkanRenderer::prepare_meshes` lets a consumer complete GPU mesh and
texture uploads during its loading phase, before beginning time-sensitive work
such as joining an authoritative game session. Loading GLBs on the CPU alone does
not complete these uploads. Resource preparation does not select or render a scene.

The API takes a span of shared immutable `Mesh` pointers. Repeated pointers
reuse the existing GPU cache. The renderer waits for its existing graphics fence,
uses the established upload/retirement path and leaves the current scene, view,
instance palettes and scene-generation count unchanged. The full span is checked
for null pointers before any upload starts. Empty input is a no-op on a live renderer.

A recoverable error leaves the displayed scene usable. Earlier successful uploads
in that batch may remain cached while their assets have external owners. Resources
without external owners retire after the normal frame fence. This is intentionally
cache preparation, not an atomic scene replacement. Upload timeout/device loss
marks the renderer fatal, and later drawing is rejected. The existing GPU resource
ownership and hardware constraints remain in force; no capacity allowance changes.

`ResourcePreparationOptions::fail_after` supports the resource consumer's failure
injection. It takes `RendererFailureStage`: vertex, index, texture, texture_upload,
descriptors, ready, upload_timeout or device_lost. Production callers retain the
`none` default. Recoverable injected failures expose `InjectedRendererFailure::stage()`;
callers must not classify exceptions using their diagnostic text.

This is a synchronous API. It does not provide a background streaming scheduler,
texture sharing between separately compiled assets, an automatic residency budget,
or responsive loading UI. Consumers choose the resources and when to prepare them;
Anima owns GPU upload, reuse, failure classification and retirement.

## CPU preparation before upload

`MeshPreparation` (in `anima/assets/mesh_preparation.hpp`) binds
CPU-prepared mip chains and material bindings to one exact immutable `Mesh`.
Its construction can run on an application worker. It preserves the existing
sRGB/data filtering, alpha coverage, sampler rules and unused-image elimination;
it does not change texture quality. The object is movable and not copyable. It
owns the source asset and temporary mip bytes, including base pixels, so consumers
should bound the number of preparations retained concurrently.

`VulkanRenderer::prepare_mesh(preparation, options)` uploads those bytes
on the render thread using the same allocation, cache and fence-retirement path.
It borrows the preparation only for the synchronous call. The application may
destroy it afterward. Reusing the same compiled asset still hits the GPU cache.
The existing span overload continues to compute mips synchronously for callers
that do not need a staged loader. Failure injection/classification is unchanged.

Moving CPU preparation to a worker does not make GPU allocation/upload
asynchronous or promise a fixed frame budget. The application schedules preparation
and bounds the number of in-flight resources.

## Verification

CTest covers CPU mip byte comparisons, cutoff isolation, unmipmapped textures
and the independent desktop consumer. The GPU resource consumer checks repeated
preparation, unchanged reference pixels, reuse during later scene selection,
rejected null inputs, recoverable failures and last-owner retirement.

After building and testing the desktop preset, run:

```sh
python3 tools/engine/gpu_resource_smoke.py build/desktop/consumer-desktop/build/consumer --fatal --output build/verification/resource-preparation
```

Use the configuration subdirectory for multi-configuration generators. The fatal
cases cover selection and preparation with injected upload timeout and device loss.
The smoke requires Vulkan validation and checks for zero warnings/errors. These
injected cases do not qualify recovery from real device loss or a hung driver.
