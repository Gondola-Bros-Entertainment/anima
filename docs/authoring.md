# Optional authoring pipeline

`python/anima_pipeline` is an optional offline asset compiler and validation
package. The C++ libraries can be built and used without this package or Blender;
applications may supply compatible GLB resources through other authoring tools.
Python is required when Anima's repository tests are enabled.

The package imports no application code or content. Python 3.10+ can use graph validation,
resource separation, preservation, bundling and receipts without Blender or
third-party runtime dependencies. Modules under `anima_pipeline.blender` run
inside Blender; export qualification currently uses Blender 5.2.1. The current
recipes require 30 Hz sampling, and animated prop actions must begin at frame 1.
These are limitations of this authoring workflow, not general runtime requirements.

The `equipment` module exports independently authored props. The `fitted` module
exports meshes for a caller-supplied skeleton and slot. Neither defines inventory
rules, item catalogs or application assets; those inputs belong to the caller.

Install with `python -m pip install /path/to/anima/python`, or put that directory
on `PYTHONPATH`. For Blender entry points, the scripts below locate the package
in their own checkout. Pin the engine checkout together with the consuming game.
The runtime remains source-consumable through public CMake targets; it is not a
binary SDK distribution.

| Public module | Responsibility |
| --- | --- |
| `graph` | Strict v2 recipes, source/composition DAG, dependency keys, semantic rig validation |
| `properties` | Explicit custom-property namespace through `AuthoringProperties` |
| `blender.sampling` | Restore frozen defaults before sampling sparse source actions |
| `blender.composition`, `blender.contact_space` | Compile declared motion owners and contact-space overlays deterministically |
| `blender.bake` | Evaluated deformation, explicit runtime hierarchy policy, rest skin and bind-preserving GLB export |
| `blender.actor` | Compile and qualify separate body, motion and fitted resources |
| `blender.fitted` | Export one fitted group without rebuilding body motion |
| `blender.equipment` | Export an independent static or animated prop and semantic marker/track declaration |
| `resources` | Split GLBs, remap accessors/nodes/channels, retain declared fitted groups |
| `preservation`, `validation` | Compare decoded geometry, shading, binds and permitted motion changes |
| `receipts` | Hash exact engine/caller compiler sources, source, recipe, evaluation dependency and output bytes |
| `bundles` | Copy nested runtime resources while excluding source/build/reference evidence |

## Consumer inputs

The consuming project owns source Blends, recipes, actor IDs, skeletons, masks,
contact chains, clips, handling/action definitions and all art direction.
`BuildContext(project_root, extra_compiler_sources)` resolves evaluation files
against an explicit root. Register every custom compiler/policy script or data
file in `extra_compiler_sources` under stable relative names. This includes
caller-selected property names, root/initial-clip settings, mesh preparation,
naming and material conversion policy; changes to those inputs must invalidate
the receipt. The package's own complete Python source closure is hashed automatically.
Compilation checks those inputs again before writing a receipt.

`compile_actor` accepts explicit `AuthoringProperties`, `root_joint`,
`initial_clip`, `material_converter`, `prepare_mesh` and `mesh_name` arguments.
Defaults use `anima.*` property names, a `root` joint and standard mesh handling.
Mesh grouping uses the configured render-group property retained in node extras,
not a project-specific object-name convention. Fitted slots are semantic strings;
the engine does not assume an inventory layout. Material conversion and subdivision
style are consumer policies, not shared compiler rules.

Body adapters may declare `preserve_hierarchy` and `preserve_object_transform`
as booleans. Hierarchy preservation retains deform-joint ancestry and requires
full inherited scale; flattening remains useful for rigs with scale-inheritance
modes that glTF cannot represent. Object-transform preservation keeps the authored
armature node, including its scale, so animated root translations remain in the
same world space as the source. Both options default to false and participate in
resource keys. Fitted exports use their body's identical settings. Independent
animated equipment always retains its authored armature object transform.

Only recipe v2, motion contract v3, actor receipt v2, attachment catalog v2 and
the declared v1 action/actor/interaction/fitted schemas are supported. There are
no old-format translators or embedded-motion fallbacks in this pipeline. The
small property/configuration adapters are explicit inputs to the current format.

## Entry points

For standard materials and the default property namespace:

```sh
blender --background --disable-autoexec --python-exit-code 1 \
  --python /path/to/anima/tools/animation/export_actor.py -- \
  --root /path/to/project --source /path/to/project/source.blend \
  --recipe /path/to/project/recipe.json --body crawler \
  --root-joint origin --out /path/to/project/build/crawler

blender --background --disable-autoexec --python-exit-code 1 \
  --python /path/to/anima/tools/animation/export_fitted.py -- \
  --root /path/to/project --source /path/to/project/source.blend \
  --recipe /path/to/project/recipe.json --body crawler --profile crawler \
  --root-joint origin --group shell --item shell --slot surface \
  --out /path/to/project/build/shell

blender --background --disable-autoexec --python-exit-code 1 \
  --python /path/to/anima/tools/animation/export_equipment.py -- \
  --source /path/to/project/tool.blend --recipe /path/to/project/tool.json \
  --out /path/to/project/build/tool
```

Actor/fitted scripts accept `--properties` for a JSON object with the dataclass
fields. Custom material or mesh policy should use the public Python functions
from a project entry point registered in `BuildContext`.
Export directories must be new. Source files remain unchanged. An actor build
contains intermediate reference/baked evidence and `compiled-fits.json` alongside
the runtime outputs enumerated by its receipt. Qualify and register fits in the
consumer catalog before publishing. Prop/fitted receipts record export evidence;
they do not imply art approval or a passed consumer's fit/contact policy.

## Independent proof

`tests/authoring/contracts.py` is part of ordinary CTest and needs no Blender,
network or game assets. It tests dependency invalidation, strict schemas,
caller/compiler/dependency/output tampering, exact splitting, nested bundles and texture/image/sampler identity after index
remapping. Changed image bytes or sampler settings fail preservation.
The public C++ consumer independently tests fitted pose ownership, residency,
invalid binds, animation rejection, attachments, actions and coordinated actors.

For the complete Blender-to-runtime check, copy `tests/authoring/consumer.py` to
a separate project directory, then run:

```sh
blender --background --disable-autoexec --python-exit-code 1 \
  --python /tmp/independent-consumer/consumer.py -- \
  --engine /path/to/anima --out /tmp/independent-consumer/project
cmake --preset headless
cmake --build --preset headless
ctest --preset headless
build/headless/consumer-assets/build/consumer --compiled-presentation \
  /tmp/independent-consumer/project
```

The project generates seven- and thirteen-joint source rigs with its own recipes,
properties, textured fits, animated props and phased actions. It checks exact clean builds,
reverse compilation order, untouched sources, and a diagnostic source edit that
changes only dependent resources. The C++ executable loads these real compiled
outputs using only public headers and libraries.

Compare authored output against the source rigs and the consuming application's
animation and rendering requirements.
