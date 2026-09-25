# Headless 3D physics

`ANIMA_BUILD_PHYSICS=ON` adds `anima::physics`, a C++20 API over pinned Jolt
5.6.0. It is OFF by default. Core-only builds neither fetch nor link Jolt. Physics
requires no assets, SDL, Vulkan or game library. With assets enabled,
`anima::physics_scene` adds scene/component and prefab bindings. All public types
are Anima-owned; Jolt headers, definitions and backend identifiers stay private.

```cpp
#include <anima/physics.hpp>
anima::physics::World world;
anima::physics::BodySettings floor;
floor.pose.position = {0, -.5F, 0};
floor.collider.half_extent = {10, .5F, 10};
auto ground = world.create(floor);
anima::physics::BodySettings sphere;
sphere.collider.shape = anima::physics::Shape::sphere;
sphere.motion = anima::physics::Motion::dynamic;
sphere.pose.position = {0, 3, 0};
auto body = world.create(sphere);
world.step(1.0 / 60);
const auto pose = body.pose();
```

Coordinates are right-handed, Y-up, in caller-consistent distance/mass units
(normally metres/kilograms); quaternions are XYZW. Capsule half-height excludes
the hemispheres. Body poses contain no scale. Mesh indices use counterclockwise
front faces and support stationary triangle geometry only. Primitive dimensions
must be at least 0.001; scalar/vector inputs are checked before entering Jolt.
Nonfinite values, degenerate triangles, invalid indices and unsupported shape
combinations fail explicitly. Vector components are bounded to ±1,000,000;
this validation bound is not a precision guarantee at large world coordinates.

## Collider geometry and mass

`Shape::convex_hull` uses `Collider::vertices` as 4–256 points, with no triangle
indices. Input points may be unordered and include interior or duplicate points
when the remaining set spans a volume. A volumetric span check requires line,
plane and out-of-plane distances of at least 0.001 before backend construction;
flat, collinear, duplicate-only and very thin sets reject. Construction can also
reject numerically unsuitable geometry. This does not decompose a concave mesh.

`Shape::compound` contains 1–64 `ColliderChild` values. Each child supplies a
primitive or convex hull and an explicit body-local `Pose`. Translation and
rotation position the child's authored geometry; there is no child scale. Mesh
children and nested compounds reject. A one-child compound supplies a collider
offset for a body. Noncompound colliders require empty `children`; compounds
require empty root vertex/index arrays. Hulls require empty indices. All scalar
fields remain validated, including fields unused by the selected shape.

```cpp
anima::physics::BodySettings assembly;
assembly.motion = anima::physics::Motion::dynamic;
assembly.mass = 4;
assembly.collider.shape = anima::physics::Shape::compound;
anima::physics::ColliderChild part;
part.pose.position = {2, 0, 0};
part.collider.half_extent = {1, .5F, .5F};
assembly.collider.children.push_back(part);
// Optional absolute body-local mass center; empty means compute from geometry.
assembly.center_of_mass = anima::Vec3{0, -.5F, 0};
auto assembly_body = world.create(assembly);
auto local_center = assembly_body.local_center_of_mass();
auto world_center = assembly_body.world_center_of_mass();
```

Geometry is compiled and owned by the body; caller collider arrays may be changed
or destroyed after creation. Shapes and mass configuration are immutable for that
body's lifetime. A compound remains one body with one motion mode, layer, sensor
flag and material. Contacts aggregate across its children into body-pair events;
leaving one child does not end a contact while another child still touches.
Queries return body identities and contact geometry, without public child IDs.

`BodySettings::mass` is total dynamic mass. Automatic centers and inertia follow
uniform-density collider volume. Compound children contribute in proportion to
volume; overlapping volumes contribute separately. `center_of_mass` optionally
specifies an **absolute body-local position**, not an offset from the calculated
center. It must be finite and within the vector bounds. It changes the rotation
center and shifts inertia with the parallel-axis rule, then scales to total mass;
it does not move the collider, object or visible mesh. Stationary triangle meshes
have no volume-based mass contract and reject an explicit override. There is no
runtime mass-center setter or independently authored inertia tensor.

`Body::pose`, construction poses, `teleport` and `move_kinematic` always refer to
the authored body origin. `local_center_of_mass` reports the accepted local center;
`world_center_of_mass` includes the body's rotation and translation. Both are
checked reads with the same lifetime rules as `pose`. Linear velocity settings,
`velocity`, `set_velocity` and `add_impulse` act at the center of mass. Rotation
can move an offset authored origin while center-of-mass linear velocity is zero.
Use the body transform and local center to distinguish those positions explicitly.

## Simulation, ownership and queries

Worlds own bodies. Copies of `Body` are checked non-owning handles, not owners.
Dropping a handle leaves the body alive. `remove()` is idempotent; it invalidates
all copies even when Jolt reuses an internal slot. All other stale-handle access
throws. Destroying a world invalidates its bodies and destroys backend state.
Multiple worlds may coexist. All worlds and their handles must be used on one
application thread; Anima owns Jolt's process registration. External Jolt global
initialization cannot be mixed with this module.

`step` accepts [0.000001, 0.1] seconds and advances one simulation step. Drive it
from the existing [fixed-step clock](../include/anima/core/fixed_step.hpp).
Large frame deltas belong in that accumulator, not a variable physics delta.
The backend uses Jolt's single-thread job system and disables body
sleeping so contact transitions remain stable. It is not a throughput or
cross-platform determinism qualification. Capacity exhaustion is reported; an
update-capacity exception does not roll back a partially advanced simulation.

Collision layers are 0–15, with all pairs enabled initially. Configure symmetric
layer pairs before creating bodies. Query filters select layer bits, optional
sensors, and one ignored body. Static/static contacts are not generated.
Sensors generate begin/end events without resolving penetration. `take_events()`
drains value snapshots after `step` or explicit removal/disable; no game callbacks
run inside the solver. Mesh subshape contacts are aggregated to body pairs.
End-event handles may already be invalid; compare their identity without
accessing their body state. Events contain no application policy or networking.

Ray and shape sweep displacements describe the full segment. Fractions are in
[0,1]; hit points and outward surface normals are world-space. Mesh queries are
two-sided (including bridge undersides). Sweep/overlap query shapes may be boxes,
spheres, capsules, convex hulls or the bounded compounds above. Query poses refer
to their authored origin, including when geometry has an offset center of mass.
The sweep translates the complete shape without rotating it during the cast.
Use `overlap` for zero-displacement clearance checks and initial
penetration. Overlap returns backend contact hits, potentially multiple per
mesh/body, with penetration depth. Applications decide which normals constitute
support, acceptable slopes and landing surfaces. No locomotion controller or
mount policy is implied. Zero-length casts are rejected, not treated as clearance.

## Scene and persistence contract

```cpp
#include <anima/physics_scene.hpp>
anima::Scene scene;
auto object = scene.create("crate");
object.set_position({0, 3, 0});
anima::physics::BodySettings settings;
settings.motion = anima::physics::Motion::dynamic;
auto rigid = object.add_component<anima::physics::RigidBody>(world, settings);
anima::ComponentCodecs codecs;
anima::physics::add_component_codec(codecs, world);
// Once per fixed tick; component callbacks and physics have explicit boundaries.
scene.fixed_update(1.0 / 60);
anima::physics::step(scene, world, 1.0 / 60);
```

A `RigidBody` combines one collider description and body, owned by its component. Construction
uses the object's world pose, ignoring `BodySettings::pose`. Dynamic bodies must
be roots; static/kinematic children support rigid parent transforms. Scaling,
shear, reflection and parenting a dynamic body are rejected. Dimensions belong
in the collider, not object scale. Compound child poses and an optional local mass
center are body-local geometry/mass settings; they do not change scene transforms.

The `step` driver accepts a Scene or SceneSet. After the caller runs component
fixed callbacks once, it validates all selected bodies, applies component
enablement and static/kinematic object poses, steps
the world, then writes dynamic body poses back to objects. Enable changes take
effect at this synchronization boundary. Teleport dynamic bodies through their
checked body handle. Disabled bodies retain state but leave collision/query
participation. Reenabled dynamic bodies resume their saved physics pose.
Every selected 3D component must belong to the supplied world, including inactive
ones. Call the driver once per world per tick; do not also call `world.step`.
Standalone bodies may share the world. The driver never calls component hooks;
after one `SceneSet::fixed_update`, the separate 2D driver can step its own world
without repeating those hooks. See [runtime coordination](scene-objects.md#additive-ownership-and-explicit-transitions)
for ordering and failure boundaries.

Removal during a component callback is safe; detached component values are
retired by Scene before the driver collects bodies. Scene teardown removes all
component-owned bodies. Destroying the world before the scene is safe; further
simulation/codec restoration rejects the expired binding. Driver entry during selected-scene callbacks/construction
is rejected. Events are consumed after the driver returns, when object and
physics poses agree.

The explicit `anima.rigid-body.v2` codec stores collider geometry, material,
motion, layer/sensor/continuous flags, optional local mass center and current
linear/angular velocity. Scene/prefab
transforms store the pose. It never stores Jolt IDs or object pointers. Prefab
instances create independent bodies; failed component restoration rolls back
created objects/bodies. Codecs reference their destination world weakly and do
not prolong it. This is a scene state format, not a deterministic solver snapshot.

The current component payload requires exactly `shape`, `extent`, `radius`,
`half_height`, `vertices`, `indices`, `children`, `center_of_mass`, `motion`,
`velocity`, `angular_velocity`, `mass`, `friction`, `restitution`, `layer`, `sensor`
and `continuous`. `center_of_mass` is either null (automatic) or a three-number
array. Each child requires exactly `position`, `rotation`, `shape`, `extent`,
`radius`, `half_height` and `vertices`; rotation is four XYZW numbers. Children
have no indices or further children. Shape enum values are box=0, sphere=1,
capsule=2, mesh=3, convex_hull=4 and compound=5. Unknown/missing fields, duplicate
keys (including escaped duplicates), invalid geometry and oversized arrays reject.
The payload limit is 16 MiB. Scene/prefab envelopes remain strict version 3;
the previous rigid-body component key is not registered or read.

This module supports the body and query types listed above. It provides no
joints, character controller, per-child material/density, runtime collider edits
or debug visualization. For independent
2D worlds, use [Box2D integration](physics2d.md).

## Qualification

`physics_world` tests body lifetime/capacity, multiple/recreated worlds,
simulation, sensors, layers, kinematic motion, primitive/mesh/hull/compound queries
and invalid geometry. Asymmetric hulls and offset/rotated compounds verify total
mass, automatic volume weighting, explicit local centers, unchanged query geometry,
rotation, teleport and kinematic authored-origin semantics. Compound contacts
verify partial and final child separation. `physics_scene` tests fixed ordering,
removal, enablement, teardown, compound/hull persistence, local mass centers and
malformed-payload rollback. Separate `consumer_physics` and
`consumer_physics_scene` projects build through public targets; `consumer_core`
rejects accidental Jolt configuration. Run:

```sh
cmake --preset headless -DANIMA_BUILD_PHYSICS=ON
cmake --build --preset headless
ctest --preset headless --output-on-failure
```

Use the [local qualification workflow](local-qualification.md) for Mac, Windows
and Linux checks. Physics tests are headless and do not exercise GPU rendering.
