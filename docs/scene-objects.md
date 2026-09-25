# Scenes, objects and shared resources

`Scene` owns live `GameObject`s. Every object has a transform, including empty
markers. A `MeshRenderer` is optional. `Mesh` resources are immutable and shared;
transform, animation pose, visibility and material tint belong to each object.
These interfaces use `anima::assets` without requiring a display or GPU.
The `Transform` value and quaternion/matrix functions are also available through
`anima::core` in `<anima/core/transform.hpp>`.

```cpp
#include <anima/scene.hpp>

auto scene = std::make_shared<anima::Scene>();
auto oak = anima::Mesh::load("oak.glb");
auto tree = scene->create("Oak", oak);
tree.transform().set({.translation = {12, 0, 8}, .scale = {2, 2, 2}});
tree.renderer().set_material_factor(0, {.8F, 1.F, .8F});

auto second = scene->create("Oak", oak);
second.transform().set_position({18, 0, 8});
auto marker = scene->create("Spawn");
marker.transform().set_position({3, 0, 4});
```

Names are labels and need not be unique. `GameObject` is a checked, copyable handle;
copying or dropping it does not create or destroy the object. Call `destroy()` to
remove an object and all its descendants. Scene destruction removes every object. Saved object, transform
and renderer views detect expired scene lifetimes and stale/reused object slots.
`valid()` is safe on a default or expired object; other operations throw on an
invalid handle. Scenes cannot be copied or moved. Access and mutation are
single-threaded, between draws.

`object.transform()` returns a checked `ObjectTransform` view. `set(Transform)`
accepts translation, quaternion rotation and scale; `set_matrix()` accepts an
exact affine matrix, preserving authored shear. `position()` and `set_position()`
read/change translation while preserving the matrix basis and current pose.
The initial matrix is identity. These setters use world coordinates;
`local_position`, `local_matrix`, `set_local_position`, `set_local` and
`set_local_matrix` use coordinates relative to the parent.

An empty object can acquire a renderer with `add_mesh(mesh)`. `remove_mesh()`
preserves the object and transform. `renderer().set_mesh(mesh)` replaces geometry
while preserving placement and resets mesh-specific pose, visibility and material
overrides. A renderer view addresses the object's current component; removal makes
it unusable until a renderer is added again. Its lifetime never exceeds the object.
Invalid replacements and pose/transform updates preserve accepted state.

`MeshRenderer::set_pose(pose)` preserves object placement. `set_material_factor`
changes only one object's linear RGB factor; clearing restores the mesh default.
Indices refer to the mesh's materials/primitives. Full shader/material replacement
is not implied by tint overrides. `set_visible` and `set_primitive_visible` preserve
resources and poses while changing presentation.

## Components and update phases

```cpp
struct Movement {
    anima::GameObject object;
    float speed = 2;
    explicit Movement(anima::GameObject owner) : object(owner) {}
    void on_fixed_update(double seconds) {
        auto position = object.position();
        position.x += speed * static_cast<float>(seconds);
        object.set_position(position);
    }
};

auto player = scene->create("Player");
auto rendering = player.add_component<anima::MeshRenderer>(character_mesh);
auto movement = player.add_component<Movement>();
movement->speed = 3;
scene->fixed_update(fixed_seconds); // caller's fixed-step clock
scene->update(frame_seconds);      // frame hooks, then late hooks
```

Components can be ordinary C++ types; no base class, reflection or global registry
is required. `add_component<T>(args...)` constructs one value of that exact type.
If its constructor accepts `GameObject` followed by the supplied arguments, the
owner is injected; otherwise only the supplied arguments are passed. Duplicate
types are rejected. Failed construction releases the type reservation. Constructor
side effects on other objects are application responsibility.

Every object has an `ObjectTransform` component, which cannot be removed or disabled.
`MeshRenderer` uses the same typed lookup and the existing render storage.
`get_component<T>()` returns a checked `ComponentRef<T>`; a missing component gives
an invalid handle. `has_component<T>`, `remove_component<T>` and
`scene.components<T>()` support typed composition and queries. No registration is
needed to attach a game-defined type. Components die with their object or scene.

Component handles become invalid after removal and stay invalid if the same type
is attached again. `operator->` pins a value through its method call, so a method
can remove itself safely. References obtained through `get()` or `operator*` are
borrowed and must not be retained across mutations. Destructors run after the
removed subtree is invalidated; values active in a method/update can survive until
that call ends. Constructors/destructors provide C++ resource ownership; there are
no implicit Awake/Start/OnDestroy callbacks. Optional activation hooks are described below.

Optional public hooks return `void` and accept `double`: `on_update`,
`on_fixed_update`, `on_late_update`. `Scene::update` runs all frame hooks, then all
late hooks. `fixed_update` runs only fixed hooks. Both use explicit caller time;
the application owns accumulation, pause, time scale and server scheduling.
Within a phase, order between components is unspecified. Each call snapshots its
participants: additions and components inactive at entry start on a later call;
removals, disablement and inherited deactivation skip remaining hooks. The Scene
must remain alive for the entire call, including lifecycle callbacks. Nested
updates are rejected. Callback exceptions propagate
after unlocking the scheduler; earlier callback effects are not rolled back.

`ComponentRef::set_enabled` controls hooks; for a MeshRenderer it also controls
visibility. `renderer().set_visible` and the component's enabled state agree.
Visibility does not inherit through the object hierarchy. The direct `transform()`
and `renderer()` helpers remain views of the object's current native capability;
typed component handles identify one particular attachment.

## Activation and lifecycle

Every object starts active. `set_active(bool)` changes its authored `active_self()`
flag; `active_in_hierarchy()` is true only when the object and every ancestor are
active. Parenting and activation publish the effective state for the entire subtree
immediately. Children retain their own flags when a parent changes. Transforms,
component lookup, explicit method calls and persistence still work while inactive.
`scene.components<T>()` includes inactive/disabled attachments so drivers can suspend
previously active resources. `ComponentRef::enabled()` remains the authored component
flag; `active()` combines it with inherited object activation.

```cpp
struct Registration {
    void on_enable() noexcept { /* enter a caller-owned registry */ }
    void on_disable() noexcept { /* leave that registry */ }
    void on_update(double seconds) { /* active frame work */ }
};
auto assembly = scene->create("assembly");
assembly.add_component<Registration>(); // no activation callbacks yet
scene->synchronize_lifecycle();          // first on_enable; no clock advancement
assembly.set_active(false);             // immediately suppresses rendering/ticks
scene->synchronize_lifecycle();          // on_disable
```

`Scene::synchronize_lifecycle()` reconciles a single snapshot. `update` and
`fixed_update` perform the same reconciliation before their phases, sharing their
participant snapshot. Optional `void on_enable() noexcept` and
`void on_disable() noexcept` hooks run once per observed effective-state transition.
The `noexcept` requirement is checked at compilation: teardown cannot recover from
a throwing cleanup callback. Fallible work belongs in explicit methods or update
hooks; callbacks that perform fallible mutations must handle their own failures.
Initial enablement waits until synchronization, so construction and prefab decoding
do not expose partially assembled objects through lifecycle hooks. Changing a flag
off and back on before synchronization coalesces to its final state.

Disables are reconciled before enables; order among unrelated attachments is
unspecified. Additions or activations made by callbacks that were not eligible in
the entry snapshot wait until another synchronization. Deactivation immediately
prevents further phase hooks, while its notification may wait until the next
boundary. Lifecycle callbacks can mutate objects/components, but cannot recursively
call scene update/synchronization. Self-removal from `on_enable` finishes that hook
before `on_disable`. No automatic draining loop or per-component ordering/dependency
graph is provided; use the explicit engine drivers for cross-system ordering.

Removal sends `on_disable` to an attachment whose enable notification was delivered,
even if it has not reached its next synchronization. Attachment/subtree invalidation
precedes cleanup; on object destruction or scene teardown, owner handles are already
invalid. Never-enabled attachments receive neither hook. Scene teardown invalidates
all handles before callbacks and disallows creation. Destructors always remain
responsible for resource release, including failed construction/loading and pinned
values whose destruction occurs after removal.

Object activation suppresses mesh drawing, visible scene bounds and diagnostic
snapshot draws without changing authored renderer/primitive visibility or resource
ownership. `Scene::instances()` still enumerates all mesh attachments;
`Instance::active` records inherited activation separately from `visible`.
Consumers implementing their own rendering must check both.

Existing explicit drivers use `ComponentRef::active()`: audio pauses/resumes sources
and excludes inactive listeners; input cancels held actions and ignores inactive
input; navigation publishes zero desired velocity without consuming routes; UI
hides inactive panels; physics excludes inactive bodies on its next fixed driver
call. Call these drivers before rendering/mixing/querying their output after changing
activation. `synchronize_lifecycle()` alone does not synchronize those systems.
Input, navigation and UI drivers accept a whole `SceneSet` as well as one `Scene`;
all drivers run between scene calls. See [runtime ownership and system ordering](runtime-lifecycle.md)
for the frame sequence and the distinct UI event callback boundary.
Physics still validates every body's world ownership and rigid transform, including
inactive bodies, and dynamic bodies must remain scene roots. There is no automatic global pause or mixed 2D/3D driver scheduler. Explicit
scene ownership and transitions are described below.

## Parenting

```cpp
auto building = scene->create("Building");
auto door = scene->create("Door", door_mesh);
door.set_parent(building, anima::ReparentMode::keep_local);
door.transform().set_local_position({2, 0, 0});
building.transform().set_position({10, 0, 4}); // door moves to {12, 0, 4}
door.clear_parent(); // preserve the door's world placement
```

`set_parent` and `clear_parent` default to `keep_world`; `keep_local` preserves
the local matrix instead. `parent()`, `children()` and `scene.roots()` return
checked handles. Any live object in the same scene can be a parent, including
empty markers. Cycles and stale/foreign parents are rejected.

World matrices, render palettes and bounds update immediately through the subtree
without resetting animation poses. Exact affine matrices preserve shear, mirrored
axes and nonuniform scale. Transform/reparent failures preserve the entire affected
subtree. World-to-local conversion requires an invertible parent; local setters and
pose-only animation updates remain usable under singular parents. Detaching with
`keep_world` does not require an inverse. Traversal/destruction is iterative, so
deep hierarchies do not consume the C++ call stack.

## Prefabs and scene documents

```cpp
#include <anima/prefab.hpp>

auto prefab = anima::Prefab::capture(building);
auto second_building = prefab.instantiate(*scene);
second_building.transform().set_position({20, 0, 4});

// Applications map shared Mesh resources to stable keys and resolve those keys.
auto text = anima::serialize_scene(*scene, name_mesh);
auto restored = anima::load_scene(text, resolve_mesh);
```

`Prefab::Node` describes a local transform, optional shared mesh, pose, renderer
overrides, authored `active` flag, object `key` and parent index. Parents precede
children; a prefab has one root. Programmatic nodes with a zero key receive unused
keys when constructing the prefab. Explicit duplicate keys reject; when copying a
node to create another object, clear its key or supply a new one and author its
reference payloads accordingly.
Capturing a nested object retains its local placement. Instantiation into a scene
or under another object multiplies the supplied placement by the authored root
matrix. Each instance owns its object/component state and shares immutable meshes.
Failed instantiation removes every object it created.

Version-3 JSON scene documents preserve forests, empty objects, hierarchy, exact
matrices, accepted pose world matrices, material factors and primitive visibility.
They also store each object's boolean `active` flag, independently of component
`enabled`. Readers and writers use version 3 exclusively; older or unknown versions
reject. Missing, nonboolean or duplicate activation fields reject.
Notification history is never persisted. Loaded components
receive their first enable at the next explicit synchronization/update, if eligible.
Meshes are external stable keys, never serialized pointers or implicitly opened
paths. A resolver runs once per distinct key. `load_scene` builds a new Scene before
returning it; the caller chooses when to replace the live scene. Documents are
limited to 16 MiB, 65,536 objects and 1,024 custom components per object.
`Prefab::serialize`/`deserialize` use the same
data model with a distinct document kind. File IO belongs to the caller.

Game-defined component state requires explicit `ComponentCodecs`:

```cpp
struct Health { int value; }; // Defined by the consuming game.
anima::ComponentCodecs codecs;
codecs.add<Health>("mygame.health.v1",
    [](const Health &health, const anima::ObjectReferences &) { return std::to_string(health.value); },
    [](anima::GameObject object, std::string_view state, const anima::ObjectReferences &) {
        object.add_component<Health>(std::stoi(std::string(state)));
    });
anima::Scene saved_scene;
auto character = saved_scene.create("Character");
character.add_component<Health>(100);
auto prefab_with_components = anima::Prefab::capture(character, codecs);
auto document = anima::serialize_scene(saved_scene, name_mesh, codecs);
auto loaded = anima::load_scene(document, resolve_mesh, codecs);
```

Codecs use stable versioned type keys and opaque UTF-8 payloads. Native transforms
and renderers have dedicated fields. Any other component without a codec causes
capture/loading to fail rather than silently lose behavior, including Animator and
FittedSet when no application persistence adapter is supplied. Runtime clocks,
external resource references, generated children and game save semantics require an
explicit application policy; they are not inferred from C++ fields. Object links
can use the checked reference context below.

Prefab validation never executes application component constructors. Instantiation
creates all native objects and links before restoring user components. Encoders
must be read-only; decoders must restrict mutations to their supplied object's
components. A decoder failure destroys the staged hierarchy. Codecs are retained
by value in a prefab, so callback captures must have appropriate lifetimes.

The ordinary `instantiate(scene, placement)` and `instantiate(parent, placement)`
overloads use that retained registry, including its physics-world, audio-mixer and
UI-host bindings. For an instance in another session, pass its configured registry:

```cpp
auto instance = prefab.instantiate(destination_scene, anima::identity(), destination_codecs);
auto nested = prefab.instantiate(destination_parent, placement, destination_codecs);
```

These overloads borrow the supplied registry only for the call. They leave the
prefab's authored data and retained registry unchanged, so subsequent ordinary
instances still use the original bindings. Every serialized component type must
exist in the destination registry before any object is created. Decoder failure
removes the staged objects and their owned resources. Placement, activation and
instance-local object-link remapping follow the same rules as ordinary
instantiation; the registry supplies services rather than changing document keys
or implicitly selecting resources from the destination scene.

## Stable object keys and component references

`GameObject::key()` returns a persistent `ObjectKey`, distinct from its runtime
`Scene::Id`. Keys are unique within one scene and unchanged by renaming, reparenting,
activation or component replacement. `Scene::find(key)` returns the corresponding
checked handle, or an invalid handle for a missing/null key. Keys are unsigned
64-bit values; zero denotes null. They are scene-scoped, not global asset IDs or
network identities. Two independent scenes may contain the same key.

Creation allocates monotonically increasing keys. Deletion, slot reuse and failed
creation/instantiation do not recycle them; allocation failure may leave gaps.
Version-3 scene documents preserve both each object's `key` and the scene's
`next_key`, including when the highest-key object or every object was deleted.
Exhaustion is represented by a zero next key and rejects further creation rather
than wrapping to a used identity. Keys are canonical decimal **strings** in JSON
to preserve all 64 bits through other tooling. `ObjectKey::string()`/`parse()` use
the same format, rejecting signs, whitespace, leading zeros, fractions and overflow.
Duplicate/null object keys, missing fields and invalid next-key bounds reject.

Use ordinary checked `GameObject` handles for runtime links. They do not retain
objects/scenes, and a removed target never becomes valid again through slot reuse.
Loading a replacement scene creates new handles; it does not rebind old ones. Use
the new scene's `find` explicitly if an application retained a persistent key.

Every codec takes a `const ObjectReferences &` argument. Codecs with object links
use it to translate checked handles:

```cpp
struct Target { anima::GameObject object; };
anima::ComponentCodecs codecs;
codecs.add<Target>("example.target.v1",
    [](const Target &target, const anima::ObjectReferences &references) {
        return references.key(target.object).string();
    },
    [](anima::GameObject owner, std::string_view data,
       const anima::ObjectReferences &references) {
        owner.add_component<Target>(references.resolve(anima::ObjectKey::parse(data)));
    });
```

`ObjectReferences::key`
maps a live target in the captured graph to its document key;
`resolve` maps that key to the newly constructed object. A default `GameObject`
maps to zero and resolves to a default handle. Stale handles, foreign/out-of-graph
targets and unresolved nonzero keys reject rather than silently becoming null.
Applications must clear intentionally removed links before capture. Component
payloads remain opaque; codecs must use this context for every object link, rather
than persisting slot IDs, names, pointers or raw keys that bypass remapping.

The entire object graph exists before component decoding. Forward, backward,
cyclic, self and cross-root links therefore resolve during scene loading. Other
components may not yet have decoded; store the target object and look up its
components after loading, for example at the first lifecycle synchronization.
Decoders may attach components only to their supplied object; do not mutate target
objects or retain borrowed component references across construction. A decoding or
reference-resolution failure rolls back all newly constructed objects/resources.

Scene loading preserves document keys. Each prefab instantiation instead allocates
fresh keys in the destination and supplies a mapping from authored keys to that
instance's objects. Independent copies point to their own children/siblings and
never to the source or another copy. Capturing a prefab includes only its subtree;
links outside it reject. Full-scene capture includes all roots, so their links work.
Single-document codecs do not resolve external objects or cross-scene links.

For explicit `ComponentCodecs::capture`/`restore` calls, supply an `ObjectReferences`
constructed from a span of live objects or explicit `{key, object}` entries. Supply
an explicit empty context for components without object links; nonnull links then
reject. Contexts are immutable and copyable,
retain only weak checked handles, and reject duplicate keys or duplicate objects.
They do not refresh after destruction or serve as a global registry. Prefab/scene
helpers construct the correctly scoped contexts automatically.

## Additive ownership and explicit transitions

`<anima/scene_set.hpp>` supplies `SceneSet`, an owner of multiple independently
loaded scenes. Each has a unique caller-supplied namespace and a weak `SceneRef`.
There is no global scene singleton, file IO, worker thread or graphics dependency.
Namespaces are nonempty strings of at most 4096 bytes without NUL; names such as
`level` and `overlay` are application data, not paths or implicit asset lookups.

```cpp
anima::SceneSet scenes;
auto level = scenes.load("level", level_document, resolve_mesh, codecs);
auto overlay = scenes.create("overlay");
overlay->create("HUD anchor");
scenes.set_active(level);
auto player = scenes.active()->create("Player");

// Snapshot all scenes before frame hooks, then run all late hooks.
scenes.update(frame_seconds);

// A complete replacement is staged before the old level is unloaded.
level = scenes.replace(level, next_level_document, resolve_mesh, codecs);
scenes.unload(overlay);
```

`create`/`load` add a scene without changing others. The first scene is selected
as `active()`; `set_active` changes only this convenient caller default. It does
not disable other scenes, run systems or select a renderer. Unloading the selected
scene selects the first remaining scene, or leaves an invalid active handle when
empty. `scenes()` returns handles in insertion order. Replacement preserves its
namespace, enumeration slot and active selection. Objects from different scenes
remain independently owned; parenting across scenes rejects.

`load` and `replace` use the same strict version-3 documents, resource resolvers and
reference-aware codecs as standalone scenes. A failed parse/resolution/decoder
releases staged resources and leaves the published scenes and selection intact.
Codecs/resolvers must obey their existing mutation boundaries; arbitrary external
callback side effects cannot be rolled back. New components receive no enable
notifications before the caller's next lifecycle synchronization. Replacement needs
capacity for both scenes while staging, including their voices/bodies/documents.

`unload`, replacement, `clear` and set destruction invalidate old scene/object/
component handles before cleanup. `clear` invalidates **every** scene before the
first component disable/destructor runs. Enabled components receive their matching
disable, and owned resources release even if renderer views remain. Currently
pinned component accesses retain only their value until the expression ends.
There is no automatic object migration or persistent-object exception to unload.

`SceneRef::get()` borrows a Scene for explicit drivers; the reference must not
outlive membership changes. `operator->` pins scene storage through its expression.
`render_scene()` supplies the existing renderer's read-only shared Scene input.
Unloading makes that view permanently empty, so renderer ownership cannot retain
live objects or resources. A renderer prepares the empty view at its next draw;
GPU retirement still obeys graphics fences and independent mesh owners. Replacement
never silently retargets a saved view. `SceneSet::render_scenes()` returns an
explicit membership snapshot for `renderer.set_scenes(set.render_scenes())`.
Refresh that selection after loading/replacement. All selected scenes render
through the same view and resource pipeline; unloading one empties its retained
view while other selected scenes keep rendering.

Membership changes and active selection are synchronous, between updates/draws.
Reentrant membership changes from loaders, lifecycle/update callbacks, component
construction and teardown reject. The set must outlive calls into its scenes and
callbacks. Component constructors also cannot enter scene scheduling while their
attachment is incomplete. Object/component mutations still follow the normal Scene
contracts. `SceneSet::update`, `fixed_update` and `synchronize_lifecycle` take one
component snapshot across all owned scenes. Disables precede enables across the
set; all frame hooks precede all late hooks. Additions or activations in another
scene during callbacks wait for the next call. Nested scheduling of any selected
scene and set membership changes reject until the call and pinned component
retirement finish. Callback exceptions unlock the entire set; prior effects are
not rolled back. Order within each phase is unspecified.

`SceneSet::components<T>()` queries attachments across the set, including disabled
or inactive components. The audio and optional physics drivers accept either one
Scene or the complete SceneSet. A typical application sequence is:

```cpp
// For each fixed tick, using the caller's accumulator:
set.fixed_update(fixed_seconds);              // component hooks exactly once
anima::physics::step(set, world3d, fixed_seconds);
anima::physics2d::step(set, world2d, fixed_seconds);
// Once per frame, after the application's fixed ticks:
set.update(frame_seconds);                   // frame, then late hooks
anima::synchronize_audio(set, audio);         // poses/enablement, no mixer time
// Then draw and render/pump audio using the application's presentation drivers.
```

Omit unused systems. Physics drivers do not run component hooks. Each backend's
components in the supplied selection must share its supplied world; standalone
bodies may share that world. Use separate sets/worlds for isolated simulations,
and never assign both physics backends to drive the same object's transform.
Audio validates one active listener across the set; active-scene selection does
not choose a listener. Drivers reject entry during selected-scene callbacks or
construction, and set drivers also reject during set loading/retirement.

Each driver validates its complete snapshot before publishing system state.
Validation failure does not undo earlier component callbacks or other systems
already advanced. Simulation/backend failures may leave that world partly
advanced; there is no transaction across worlds. Application code still owns
clocks, pause, transition policy and event handling. There is no automatic
asynchronous streaming scheduler.

`address(object)` returns `SceneAddress{scene_namespace, object_key}` for a live
object owned by the set. `find(address)` performs an **explicit** lookup and returns
an invalid object for an unloaded namespace or missing/null key. Two scenes may
safely contain the same ObjectKey. Runtime handles never rebind after unload;
explicit address lookup may select a newly loaded scene using that namespace.
Addresses do not keep targets alive. Single-scene/prefab codecs still resolve only
their own captured graph and reject external links. Whole-set persistence supplies
a shared reference context across the captured scenes; loading never rebinds
existing handles.

## Persisting a complete scene set

```cpp
const auto saved = scenes.serialize(name_mesh, codecs);
// Decode every scene and component before replacing any live membership.
scenes.restore(saved, resolve_mesh, destination_codecs);
auto restored_player = scenes.find(player_address); // explicit new identity lookup
renderer.set_scenes(scenes.render_scenes());        // refresh retained selection
```

`SceneSet::serialize` captures namespaces, insertion order, active selection and
every scene's objects, hierarchy, local object keys and next-key allocator state.
Component adapters receive one `ObjectReferences` context for the entire set.
The existing `references.key(object)` and `references.resolve(key)` calls support
forward, backward, cyclic and self links across scenes, even when different scenes
have identical local object keys. Null remains zero. Links to stale objects or
objects outside the captured set reject; runtime IDs and pointers are never saved.

The strict version-1 `anima.scene-set` envelope has `version`, `kind`, `active`,
`scenes` and `references` fields. Each scene entry contains `key` (namespace),
`next_key` and `objects`. Object records use the existing version-3 scene node
representation. Each reference row contains `key` (operation-wide document key),
`scene` (namespace) and `object` (scene-local key). All object/reference keys are
canonical decimal strings. The table must map every captured object exactly once;
null/duplicate keys, duplicate destinations and missing targets reject. Capture
assigns deterministic operation keys; they are scoped to that one document and
are distinct from scene-local persistent identities.

The inner object arrays are part of the set document and use its shared reference
table. They cannot be extracted and loaded as standalone scene documents. Ordinary
scene/prefab documents remain strict version 3 and keep their existing local
reference scope. Component payloads stay opaque; codecs must use the provided
context instead of storing `object.key()` directly for linked objects.

Namespaces must be unique and obey the normal scene-set limits. `active` is null
exactly when the set is empty; otherwise it names a member. A whole document is
limited to 16 MiB, 1,024 scenes and 65,536 total objects, retaining existing node,
component and resource-key limits. Mesh naming is checked across the complete set:
two distinct meshes cannot claim one key. Loading resolves each mesh key once,
allowing scenes to retain shared immutable resources.

`restore` borrows the destination registry and stages a private set. Every native
object, transform, hierarchy and renderer exists before any component decoder
runs, so codecs can resolve links into later scenes. Other components may not yet
be decoded: store linked object handles and inspect their components after loading.
Initial lifecycle notifications wait for the next explicit synchronization/update.
Codecs may attach components only to their supplied object; encoders/resource
callbacks are read-only and must obey the normal persistence mutation boundary.

Failure leaves the previous membership, active scene and object handles intact.
Staged identities are invalidated together before releasing staged components and
their resources. Success publishes the complete new membership and active selection,
then invalidates every old scene/object handle before old component cleanup. Saved
render views become empty; refresh renderer selection explicitly. Restore requires
capacity for both the old and staged bodies, voices and documents. Arbitrary external
side effects from user callbacks are not transactional.

Set serialization/restoration is synchronous and rejects nested scheduling or
membership changes. Cross-scene references in a complete set document do not imply
automatic repair after unloading/replacing just one scene, persistent-object
migration, prefab nesting/variants or asynchronous streaming.

## Cameras

Scene-facing perspective/orthographic cameras and persisted view selection use
these same objects, transforms, activation flags and reference-aware codecs. See
[scene cameras](cameras.md) for resolution and renderer integration.

## Terrain

```cpp
#include <anima/terrain.hpp>

auto data = std::make_shared<anima::TerrainData>(
    anima::Heightfield{2, 2, 0, 0, 1, 1, {0, 0, 0, 1}});
anima::Terrain terrain(data);
auto ground = scene->create("Ground", terrain.mesh());
auto sample = terrain.data().sample(.75F, .25F);
```

`TerrainData` owns and validates height samples. Queries use the data's coordinates,
independently of object presentation. A visual object transform does not move the
query field. Applications using the same field for collision should keep matching
coordinates or explicitly convert queries.

`Terrain::compile(HeightfieldView, TerrainAppearance)` supports borrowed samples
owned by an existing loader. Appearance supplies optional per-sample normals and
colours, UV scale, materials/textures and a node label. Inputs are consumed during
compilation, never retained as borrowed references. Missing normals are computed;
missing colours are white. Rendering and height queries use the same diagonal.

`TerrainGrid` additionally accepts a double-precision global sample origin/spacing
and integer column/row offsets. Each coordinate is calculated from the common
lattice before conversion to float, so neighbouring chunks share identical boundary
positions. World-cell selection, file formats and residency policy belong to the
application.

## Animation

```cpp
#include <anima/animation.hpp>

auto source = anima::load_asset("character.glb");
auto actor = scene->create("Character", anima::Mesh::compile(*source));
auto animator = actor.add_component<anima::Animator>(source);
animator->play("Walk", true);
scene->update(seconds);
auto events = animator->events();
actor.transform().set_position({4, 0, 2}); // keeps the sampled pose
```

The Animator retains its animation source and checks the node hierarchy and rest
bind against the mesh. Time is explicit; no global update loop runs automatically.
`select(ClipMetadata)` adds authored loop/event policy, while pause/resume, seek,
restart and bind pose support preview workflows. Replacing the object's mesh
requires an explicit new Animator binding. Each Animator has its own playback.
`pose()` exposes its last successfully published pose.
As an attached component, Animator advances in the frame phase; `events()` exposes
events from its most recent scene-driven update, without an accumulating queue.
A standalone Animator can still be driven with `update` and consume its returned
events directly. Choose one clock/driver for each Animator.

Advanced animation uses the existing motion/action/interaction APIs and publishes
their evaluated pose through the same MeshRenderer. Game authority, action choices,
equipment eligibility and movement timing remain application-owned. See
[animation evaluation](animation-evaluation.md).

## Fitted meshes

```cpp
#include <anima/assets/fitted.hpp>

auto clothing = actor.add_component<anima::FittedSet>(fitted_library);
clothing->replace({"linen.shirt", "leather.boots"});
scene->update(seconds); // Animator runs first; fitted pose synchronization runs late.
```

`FittedSet` binds a fitted asset library to a body's current mesh and owns its
fitted objects. The library must match the body's node hierarchy and rest bind.
New items inherit the body's accepted pose, placement and visibility immediately.
Replacement retains unchanged objects and rolls back new objects on failure.
Multiple sets share immutable fitted meshes while keeping independent poses.

`sync()` copies the body's final pose, placement and visibility. An attached set
runs it during late update; an authoritative application can call it after its own
pose evaluation instead. Hidden sets defer pose work until visible again. The set
reads accepted scene state, so applications do not need another fitted pose cache.

Fitted objects are children of the body. Removing the component removes them;
destroying the body removes both the component and its descendants. Standalone
sets remain supported and are safe to destroy after their body or scene expires.
Mutation rejects expired bodies, replaced body meshes and externally removed or
replaced fitted renderers. Replacing a body mesh requires a new set binding.
Game inventory, slot eligibility, body-part coverage and boot-height rules remain
application policy.

`AttachmentSet::add(owner)` similarly installs held meshes as body-owned children,
initially placed at their sockets with the owner's visibility. Preparation and
failed addition preserve prior membership; repeated addition is rejected. Advanced
prop animation/contact evaluators supply final socket poses through the same
checked ownership APIs.

## Resource preparation and rendering

`Mesh::compile` builds a shared resource from an imported or programmatic `Asset`.
`Mesh::compile_static` can split static geometry under a caller-selected expanded
vertex limit and reduce texture dimensions with the existing mip/filter policy.
It preserves source data and remaps each chunk's material/texture references.
`max_vertices=0` leaves geometry unsplit; `max_texture_edge=0` preserves resolution.
A nonzero vertex limit must accommodate at least one triangle.

`VulkanRenderer::set_scenes({scene})` or `RendererOptions::scenes` selects live
scenes. Rendering reads their existing indexed resource storage; there is no second scene or
rendering path behind the GameObject interface. `Scene::Id` and explicit scene
operations remain useful for engine subsystems that maintain compact membership.
`scene.size()` counts all objects; `scene.instances()` enumerates renderable ones.

CPU compilation does not mean GPU residency. `MeshPreparation` builds CPU mip
chains; `prepare_mesh`/`prepare_meshes` perform synchronous renderer-side preparation.
Uploads can fail, and GPU resources retire through the existing frame-fence path.
See [resource preparation](resource-preparation.md) and
[resource ownership](render-resource-architecture.md). No frame-time deadline or
asynchronous upload guarantee is implied by the object API.

The independent consumers in `tests/consumer/scene_objects.hpp`, `lifecycle.hpp` and `components.hpp`
exercise lifetime, parenting, prefab/scene roundtrips, component mutation during
updates, terrain, static preparation and animation without game assets.
`tests/consumer/presentation.hpp` checks fitted membership, late-phase pose/visibility
inheritance, replacement failure and cleanup against generated actor rigs.
The desktop consumer renders and updates parented objects through the same public API.
